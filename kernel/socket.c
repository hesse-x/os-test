#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "kernel/socket.h"
#include "kernel/proc.h"
#include "kernel/serial.h"
#include "kernel/trap.h"
#include "kernel/mem/slab.h"
#include "kernel/spinlock.h"
#include "common/errno.h"
#include "common/socket.h"
#include "arch/x64/utils.h"
#include "arch/x64/apic.h"

// ===================== Global socket lock =====================
spinlock_t socket_lock = {0};

// ===================== Bind name space =====================
static struct unix_bind_entry *unix_bind_table[UNIX_HASH_SIZE];

static uint32_t unix_hash(const char *path) {
    uint32_t h = 0;
    for (int i = 0; path[i] && i < 108; i++) {
        h = h * 31 + (uint8_t)path[i];
    }
    return h % UNIX_HASH_SIZE;
}

int unix_bind_lookup(const char *sun_path, struct unix_sock **out) {
    uint32_t h = unix_hash(sun_path);
    for (struct unix_bind_entry *e = unix_bind_table[h]; e; e = e->next) {
        int i = 0;
        while (i < 108 && sun_path[i] == e->sun_path[i]) {
            if (sun_path[i] == '\0') break;
            i++;
        }
        if (i < 108 && sun_path[i] == '\0' && e->sun_path[i] == '\0') {
            if (e->sock && e->sock->state == UNIX_LISTEN) {
                *out = e->sock;
                return 0;
            }
            return -ENOENT;  // exists but not listening
        }
    }
    return -ENOENT;
}

int unix_bind_register(const char *sun_path, struct unix_sock *sock) {
    // Check for duplicate (already bound to some socket — not necessarily LISTEN)
    uint32_t h = unix_hash(sun_path);
    for (struct unix_bind_entry *e = unix_bind_table[h]; e; e = e->next) {
        int i = 0;
        while (i < 108 && sun_path[i] == e->sun_path[i]) {
            if (sun_path[i] == '\0') break;
            i++;
        }
        if (i < 108 && sun_path[i] == '\0' && e->sun_path[i] == '\0') {
            return -EADDRINUSE;
        }
    }

    struct unix_bind_entry *entry = (struct unix_bind_entry *)kmalloc(sizeof(struct unix_bind_entry));
    if (!entry) return -ENOMEM;

    int i = 0;
    while (sun_path[i] && i < 107) {
        entry->sun_path[i] = sun_path[i];
        i++;
    }
    entry->sun_path[i] = '\0';
    entry->sock = sock;
    entry->next = unix_bind_table[h];
    unix_bind_table[h] = entry;
    return 0;
}

void unix_bind_unregister(struct unix_sock *sock) {
    if (!sock->sun_path[0]) return;  // not bound
    uint32_t h = unix_hash(sock->sun_path);
    struct unix_bind_entry **pp = &unix_bind_table[h];
    while (*pp) {
        struct unix_bind_entry *e = *pp;
        if (e->sock == sock) {
            *pp = e->next;
            kfree(e);
            return;
        }
        pp = &e->next;
    }
}

// ===================== sk_buff allocation =====================

struct sk_buff *skb_alloc(uint32_t data_len) {
    struct sk_buff *skb = (struct sk_buff *)kmalloc(sizeof(struct sk_buff) + data_len);
    if (!skb) return NULL;
    skb->next = NULL;
    skb->len = data_len;
    skb->consumed = 0;
    skb->num_fds = 0;
    return skb;
}

void skb_free(struct sk_buff *skb) {
    if (skb) kfree(skb);
}

void skb_enqueue(struct unix_sock *sock, struct sk_buff *skb) {
    skb->next = NULL;
    if (sock->recv_queue_tail) {
        sock->recv_queue_tail->next = skb;
    } else {
        sock->recv_queue_head = skb;
    }
    sock->recv_queue_tail = skb;
    sock->recv_queue_len++;
}

struct sk_buff *skb_dequeue(struct unix_sock *sock) {
    if (!sock->recv_queue_head) return NULL;
    struct sk_buff *skb = sock->recv_queue_head;
    sock->recv_queue_head = skb->next;
    if (!sock->recv_queue_head) {
        sock->recv_queue_tail = NULL;
    }
    skb->next = NULL;
    sock->recv_queue_len--;
    return skb;
}

// ===================== unix_sock allocation =====================

struct unix_sock *unix_sock_alloc(void) {
    struct unix_sock *sock = (struct unix_sock *)kmalloc(sizeof(struct unix_sock));
    if (!sock) return NULL;
    sock->state = UNIX_FREE;
    sock->peer = -1;
    sock->ref_count = 1;
    sock->recv_queue_head = NULL;
    sock->recv_queue_tail = NULL;
    sock->recv_queue_len = 0;
    sock->blocked_reader = -1;
    sock->blocked_writer = -1;
    sock->backlog_head = NULL;
    sock->backlog_tail = NULL;
    sock->backlog_len = 0;
    sock->backlog_max = 0;
    sock->shutdown_read = 0;
    sock->shutdown_write = 0;
    sock->sun_path[0] = '\0';
    return sock;
}

void unix_sock_free(struct unix_sock *sock) {
    if (!sock) return;
    // Free any remaining skbs in recv queue
    while (sock->recv_queue_head) {
        struct sk_buff *skb = skb_dequeue(sock);
        skb_free(skb);
    }
    // Free backlog connections
    struct unix_sock *bp = sock->backlog_head;
    while (bp) {
        struct unix_sock *next = bp->backlog_head;  // backlog_head is next in chain
        // Tell peer we're closing
        if (bp->peer >= 0) {
            proc_t *peer = &procs[bp->peer];
            if (peer->pid == bp->peer) {
                // Wake peer if waiting on this socket
                sock_wake_reader(bp);
                sock_wake_writer(bp);
            }
        }
        // Free the child socket
        bp->peer = -1;
        bp->state = UNIX_CLOSED;
        // Free any skbs
        while (bp->recv_queue_head) {
            struct sk_buff *skb2 = skb_dequeue(bp);
            skb_free(skb2);
        }
        kfree(bp);
        bp = next;
    }
    kfree(sock);
}

void unix_sock_acquire(struct unix_sock *sock) {
    if (sock) sock->ref_count++;
}

void unix_sock_release(struct unix_sock *sock) {
    if (!sock) return;
    sock->ref_count--;
    if (sock->ref_count <= 0) {
        unix_bind_unregister(sock);
        unix_sock_free(sock);
    }
}

// ===================== Wake helpers =====================

void sock_wake_reader(struct unix_sock *sock) {
    if (sock->blocked_reader >= 0) {
        wake_process(sock->blocked_reader);
    }
}

void sock_wake_writer(struct unix_sock *sock) {
    if (sock->blocked_writer >= 0) {
        wake_process(sock->blocked_writer);
    }
}

// ===================== Internal sendmsg/recvmsg =====================

int64_t sock_sendmsg_internal(struct unix_sock *sock,
                               const struct iovec *iov, size_t iovlen,
                               const void *control, size_t controllen,
                               int flags) {
    // Check shutdown_write
    if (sock->shutdown_write) return -EPIPE;

    // Calculate total data length
    uint32_t total = 0;
    for (size_t i = 0; i < iovlen; i++) {
        total += iov[i].iov_len;
    }
    if (total > MAX_SOCKET_DATA) return -EMSGSIZE;

    // Allocate skb
    struct sk_buff *skb = skb_alloc(total);
    if (!skb) return -ENOMEM;

    // Copy data from iovecs
    uint32_t offset = 0;
    for (size_t i = 0; i < iovlen; i++) {
        if (iov[i].iov_base && iov[i].iov_len > 0) {
            __memcpy(skb->data + offset, iov[i].iov_base, iov[i].iov_len);
            offset += iov[i].iov_len;
        }
    }

    // Process control data (SCM_RIGHTS)
    if (control && controllen >= sizeof(struct cmsghdr)) {
        uint8_t *cmsg_ptr = (uint8_t *)control;
        uint8_t *cmsg_end = cmsg_ptr + controllen;
        while (cmsg_ptr + sizeof(struct cmsghdr) <= cmsg_end) {
            struct cmsghdr *cmsg = (struct cmsghdr *)cmsg_ptr;
            uint32_t aligned_len = CMSG_ALIGN(cmsg->cmsg_len);
            if (aligned_len > (uint32_t)(cmsg_end - cmsg_ptr)) break;
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
                int num_fds = (cmsg->cmsg_len - CMSG_ALIGN(sizeof(struct cmsghdr))) / sizeof(int);
                if (num_fds > SCM_MAX_FD) {
                    skb_free(skb);
                    return -EINVAL;
                }
                if (num_fds > 0) {
                    int *fds = (int *)CMSG_DATA(cmsg);
                    for (int i = 0; i < num_fds; i++) {
                        if (fds[i] < 0 || fds[i] >= MAX_FD ||
                            current_proc->fd_table[fds[i]].type == FD_NONE) {
                            skb_free(skb);
                            return -EBADF;
                        }
                        skb->fds[skb->num_fds++] = fds[i];
                    }
                }
            } else {
                skb_free(skb);
                return -EINVAL;
            }
            cmsg_ptr += aligned_len;
        }
    }

    // Find peer socket
    pid_t peer_pid = sock->peer;
    if (peer_pid < 0 || peer_pid >= MAX_PROC) {
        skb_free(skb);
        return -ENOTCONN;
    }
    proc_t *peer_proc = &procs[peer_pid];
    if (peer_proc->pid != peer_pid) {
        skb_free(skb);
        return -ENOTCONN;
    }

    // Lock: find the socket fd in peer's fd_table
    // We need the peer's unix_sock. We stored it in sock->peer, but the peer might
    // have it in its fd_table. We'll iterate peer's fd_table to find the matching
    // unix_sock that has sock as its peer.
    // Actually, let's store a direct pointer: we need to find the peer's unix_sock.
    // The peer is identified by sock->peer (PID). But we need the struct unix_sock*.
    // We'll search peer's fd_table for an FD_SOCKET whose unix_sock has peer == current PID.
    spin_lock(&socket_lock);

    struct unix_sock *peer_sock = NULL;
    for (int fd = 0; fd < MAX_FD; fd++) {
        if (peer_proc->fd_table[fd].type == FD_SOCKET) {
            struct unix_sock *ps = peer_proc->fd_table[fd].sock;
            if (ps && ps->peer == current_proc->pid && ps->state == UNIX_CONNECTED) {
                peer_sock = ps;
                break;
            }
        }
    }

    if (!peer_sock) {
        spin_unlock(&socket_lock);
        skb_free(skb);
        return -EPIPE;
    }

    // Check peer shutdown
    if (peer_sock->shutdown_read) {
        spin_unlock(&socket_lock);
        skb_free(skb);
        return -EPIPE;
    }

    // If O_NONBLOCK and queue is "full" (more than a reasonable limit), return EAGAIN
    if ((flags & MSG_DONTWAIT) && peer_sock->recv_queue_len > 128) {
        spin_unlock(&socket_lock);
        skb_free(skb);
        return -EAGAIN;
    }

    // Enqueue
    skb_enqueue(peer_sock, skb);
    pid_t blocked_reader = peer_sock->blocked_reader;
    spin_unlock(&socket_lock);

    // Wake reader outside socket_lock
    if (blocked_reader >= 0) {
        wake_process(blocked_reader);
    }

    return (int64_t)total;
}

int64_t sock_recvmsg_internal(struct unix_sock *sock,
                               const struct iovec *iov, size_t iovlen,
                               void *control, size_t *controllen,
                               int flags) {
    bool nonblock = (flags & MSG_DONTWAIT) != 0;

    while (1) {
        spin_lock(&socket_lock);

        // Check recv queue
        struct sk_buff *skb = sock->recv_queue_head;

        if (!skb) {
            // Queue empty: check EOF conditions
            if (sock->shutdown_read ||
                sock->state == UNIX_CLOSED ||
                (sock->peer >= 0 && procs[sock->peer].pid == sock->peer &&
                 procs[sock->peer].state == ZOMBIE)) {
                // Check if peer has closed
                bool peer_closed = true;
                if (sock->peer >= 0 && sock->peer < MAX_PROC) {
                    proc_t *pp = &procs[sock->peer];
                    if (pp->pid == sock->peer) {
                        // Scan peer's fd_table for a socket pointing back to us
                        for (int fd = 0; fd < MAX_FD; fd++) {
                            if (pp->fd_table[fd].type == FD_SOCKET &&
                                pp->fd_table[fd].sock &&
                                pp->fd_table[fd].sock->peer == current_proc->pid) {
                                peer_closed = false;
                                break;
                            }
                        }
                    }
                }
                if (peer_closed) {
                    spin_unlock(&socket_lock);
                    return 0;  // EOF
                }
            }

            if (nonblock) {
                spin_unlock(&socket_lock);
                return -EAGAIN;
            }

            // Block reader
            sock->blocked_reader = current_proc->pid;
            proc_t *proc = current_proc;
            proc->state = BLOCKED;
            proc->wait_event = WAIT_POLL;
            spin_unlock(&socket_lock);
            schedule();
            spin_lock(&socket_lock);
            sock->blocked_reader = -1;
            // Re-try after wake
            spin_unlock(&socket_lock);
            continue;
        }

        // We have data. Calculate how much to read.
        uint32_t avail = skb->len - skb->consumed;
        uint32_t to_read = 0;
        for (size_t i = 0; i < iovlen && to_read < avail; i++) {
            uint32_t copy = (uint32_t)iov[i].iov_len;
            if (copy > avail - to_read) copy = avail - to_read;
            to_read += copy;
        }

        // Copy data to iovecs
        uint32_t data_offset = skb->consumed;
        uint32_t remaining = to_read;
        for (size_t i = 0; i < iovlen && remaining > 0; i++) {
            if (iov[i].iov_base && iov[i].iov_len > 0) {
                uint32_t copy = (uint32_t)iov[i].iov_len;
                if (copy > remaining) copy = remaining;
                __memcpy(iov[i].iov_base, skb->data + data_offset, copy);
                data_offset += copy;
                remaining -= copy;
            }
        }

        skb->consumed += to_read;
        bool skb_consumed = (skb->consumed >= skb->len);

        // Process SCM_RIGHTS (lazy install — only when skb fully consumed, or at first recvmsg)
        int num_fds_installed = 0;
        int installed_fds[SCM_MAX_FD];
        if (skb->num_fds > 0 && skb_consumed) {
            proc_t *proc = current_proc;
            for (int i = 0; i < skb->num_fds && num_fds_installed < SCM_MAX_FD; i++) {
                int orig_fd = skb->fds[i];
                // Find free fd slot in receiver
                int new_fd = -1;
                for (int f = 3; f < MAX_FD; f++) {
                    if (proc->fd_table[f].type == FD_NONE) {
                        new_fd = f;
                        break;
                    }
                }
                if (new_fd < 0) break;

                // Copy the sender's fd entry (validate it still exists)
                pid_t sender_pid = sock->peer;
                if (sender_pid >= 0 && sender_pid < MAX_PROC) {
                    proc_t *sender = &procs[sender_pid];
                    if (sender->pid == sender_pid && orig_fd >= 0 && orig_fd < MAX_FD) {
                        proc->fd_table[new_fd] = sender->fd_table[orig_fd];
                        // Bump ref counts
                        if (proc->fd_table[new_fd].type == FD_PIPE && proc->fd_table[new_fd].pipe) {
                            proc->fd_table[new_fd].pipe->ref_count++;
                        } else if (proc->fd_table[new_fd].type == FD_SHM && proc->fd_table[new_fd].shm) {
                            shm_get(proc->fd_table[new_fd].shm);
                        } else if (proc->fd_table[new_fd].type == FD_FILE) {
                            proc->fd_table[new_fd].file_data.ref_count++;
                        }
                        // FD_SOCKET: acquire ref
                        if (proc->fd_table[new_fd].type == FD_SOCKET && proc->fd_table[new_fd].sock) {
                            unix_sock_acquire(proc->fd_table[new_fd].sock);
                        }
                        installed_fds[num_fds_installed++] = new_fd;
                    }
                }
            }
        }

        // Remove consumed skb
        if (skb_consumed) {
            skb_dequeue(sock);
            skb_free(skb);
        }

        pid_t blocked_writer = sock->blocked_writer;
        spin_unlock(&socket_lock);

        // Wake writer outside lock
        if (blocked_writer >= 0) {
            wake_process(blocked_writer);
        }

        // Write SCM_RIGHTS to control buffer
        if (num_fds_installed > 0 && control && controllen && *controllen > 0) {
            struct cmsghdr *cmsg = (struct cmsghdr *)control;
            size_t needed = CMSG_ALIGN(sizeof(struct cmsghdr)) + num_fds_installed * sizeof(int);
            if (*controllen >= needed) {
                cmsg->cmsg_len = (uint32_t)(CMSG_ALIGN(sizeof(struct cmsghdr)) + num_fds_installed * sizeof(int));
                cmsg->cmsg_level = SOL_SOCKET;
                cmsg->cmsg_type = SCM_RIGHTS;
                int *out_fds = (int *)CMSG_DATA(cmsg);
                for (int i = 0; i < num_fds_installed; i++) {
                    out_fds[i] = installed_fds[i];
                }
                *controllen = cmsg->cmsg_len;
            } else {
                *controllen = 0;
            }
        } else if (control && controllen) {
            *controllen = 0;
        }

        return (int64_t)to_read;
    }
}

// ===================== socket close / cleanup =====================

void sock_close(struct unix_sock *sock) {
    if (!sock) return;

    spin_lock(&socket_lock);
    sock->shutdown_read = 1;
    sock->shutdown_write = 1;
    sock->state = UNIX_CLOSED;

    // Free all skbs in recv queue
    while (sock->recv_queue_head) {
        struct sk_buff *skb = skb_dequeue(sock);
        skb_free(skb);
    }

    // Wake blocked reader/writer and notify peer
    pid_t reader = sock->blocked_reader;
    pid_t writer = sock->blocked_writer;
    pid_t peer_pid = sock->peer;
    sock->blocked_reader = -1;
    sock->blocked_writer = -1;

    spin_unlock(&socket_lock);

    // Wake our blocked reader/writer outside lock
    if (reader >= 0) wake_process(reader);
    if (writer >= 0) wake_process(writer);

    // Wake peer process so it can detect EOF/EPIPE
    if (peer_pid >= 0) wake_process(peer_pid);

    // Release reference (actual free when ref_count hits 0)
    unix_sock_release(sock);
}

// ===================== Syscall implementations =====================

uint64_t sys_socket(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t _u1, uint64_t _u2, uint64_t _u3) {
    int domain = (int)arg1;
    int type = (int)arg2;
    int protocol = (int)arg3;

    // Only AF_UNIX SOCK_STREAM supported
    if (domain != AF_UNIX) return (uint64_t)-EAFNOSUPPORT;
    if (type != SOCK_STREAM) return (uint64_t)-EPROTONOSUPPORT;
    if (protocol != 0) return (uint64_t)-EPROTONOSUPPORT;

    struct unix_sock *sock = unix_sock_alloc();
    if (!sock) return (uint64_t)-ENOMEM;

    proc_t *proc = current_proc;

    spin_lock(&socket_lock);

    // Find free fd slot
    int fd = -1;
    for (int i = 3; i < MAX_FD; i++) {
        if (proc->fd_table[i].type == FD_NONE) {
            fd = i;
            break;
        }
    }
    if (fd < 0) {
        spin_unlock(&socket_lock);
        unix_sock_release(sock);
        return (uint64_t)-EMFILE;
    }

    proc->fd_table[fd].type = FD_SOCKET;
    proc->fd_table[fd].flags = O_RDWR;
    proc->fd_table[fd].sock = sock;
    sock->state = UNIX_FREE;

    spin_unlock(&socket_lock);

    return (uint64_t)fd;
}

uint64_t sys_bind(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t _u1, uint64_t _u2, uint64_t _u3) {
    int fd = (int)arg1;
    const struct sockaddr_un *addr = (const struct sockaddr_un *)arg2;
    socklen_t addrlen = (socklen_t)arg3;

    proc_t *proc = current_proc;

    // Validate fd
    if (fd < 0 || fd >= MAX_FD) return (uint64_t)-EBADF;
    if (proc->fd_table[fd].type != FD_SOCKET) return (uint64_t)-ENOTSOCK;

    // Validate addr
    uint64_t addr_ptr = (uint64_t)addr;
    if (!addr_ptr || addr_ptr >= 0xFFFFFFFF80000000ULL ||
        addr_ptr + addrlen > 0xFFFFFFFF80000000ULL || addrlen <= 0)
        return (uint64_t)-EFAULT;

    // Read sun_family
    uint16_t sun_family;
    __memcpy(&sun_family, addr, sizeof(sun_family));
    if (sun_family != AF_UNIX) return (uint64_t)-EAFNOSUPPORT;

    // Read sun_path (max 108 bytes)
    char sun_path[108];
    __memset(sun_path, 0, sizeof(sun_path));
    size_t path_len = addrlen - sizeof(sun_family);
    if (path_len > 107) path_len = 107;
    if (path_len > 0)
        __memcpy(sun_path, (const char *)addr + sizeof(sun_family), path_len);
    sun_path[107] = '\0';

    if (sun_path[0] == '\0') return (uint64_t)-EINVAL;  // empty path

    struct unix_sock *sock = proc->fd_table[fd].sock;
    if (!sock) return (uint64_t)-EBADF;

    spin_lock(&socket_lock);

    if (sock->state != UNIX_FREE) {
        spin_unlock(&socket_lock);
        return (uint64_t)-EINVAL;
    }

    // Register in name space
    int ret = unix_bind_register(sun_path, sock);
    if (ret != 0) {
        spin_unlock(&socket_lock);
        return (uint64_t)(-ret);
    }

    // Save sun_path in sock
    int i = 0;
    while (sun_path[i] && i < 107) {
        sock->sun_path[i] = sun_path[i];
        i++;
    }
    sock->sun_path[i] = '\0';

    spin_unlock(&socket_lock);

    return 0;
}

uint64_t sys_listen(uint64_t arg1, uint64_t arg2, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4) {
    int fd = (int)arg1;
    int backlog = (int)arg2;

    proc_t *proc = current_proc;

    if (fd < 0 || fd >= MAX_FD) return (uint64_t)-EBADF;
    if (proc->fd_table[fd].type != FD_SOCKET) return (uint64_t)-ENOTSOCK;

    struct unix_sock *sock = proc->fd_table[fd].sock;
    if (!sock) return (uint64_t)-EBADF;

    if (backlog <= 0) backlog = 1;
    if (backlog > UNIX_MAX_BACKLOG) backlog = UNIX_MAX_BACKLOG;

    spin_lock(&socket_lock);
    sock->state = UNIX_LISTEN;
    sock->backlog_max = backlog;
    spin_unlock(&socket_lock);

    return 0;
}

uint64_t sys_accept(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t _u1, uint64_t _u2, uint64_t _u3) {
    int fd = (int)arg1;
    struct sockaddr_un *addr = (struct sockaddr_un *)arg2;
    socklen_t *addrlen = (socklen_t *)arg3;

    proc_t *proc = current_proc;

    if (fd < 0 || fd >= MAX_FD) return (uint64_t)-EBADF;
    if (proc->fd_table[fd].type != FD_SOCKET) return (uint64_t)-ENOTSOCK;

    struct unix_sock *listen_sock = proc->fd_table[fd].sock;
    if (!listen_sock) return (uint64_t)-EBADF;

    // Validate addr pointers if non-null
    if (addr) {
        uint64_t aptr = (uint64_t)addr;
        if (aptr >= 0xFFFFFFFF80000000ULL) return (uint64_t)-EFAULT;
    }
    if (addrlen) {
        uint64_t alptr = (uint64_t)addrlen;
        if (alptr >= 0xFFFFFFFF80000000ULL || alptr + sizeof(socklen_t) > 0xFFFFFFFF80000000ULL)
            return (uint64_t)-EFAULT;
    }

    while (1) {
        spin_lock(&socket_lock);

        if (listen_sock->state != UNIX_LISTEN) {
            spin_unlock(&socket_lock);
            return (uint64_t)-EINVAL;
        }

        struct unix_sock *child = listen_sock->backlog_head;

        if (!child) {
            // No pending connections: block
            listen_sock->blocked_reader = proc->pid;
            proc->state = BLOCKED;
            proc->wait_event = WAIT_POLL;
            spin_unlock(&socket_lock);
            schedule();
            spin_lock(&socket_lock);
            listen_sock->blocked_reader = -1;
            spin_unlock(&socket_lock);
            continue;  // re-check backlog after wake
        }

        // Dequeue from backlog
        listen_sock->backlog_head = child->backlog_head;
        if (!listen_sock->backlog_head) {
            listen_sock->backlog_tail = NULL;
        }
        listen_sock->backlog_len--;

        // Allocate new fd for this child socket
        int new_fd = -1;
        for (int i = 3; i < MAX_FD; i++) {
            if (proc->fd_table[i].type == FD_NONE) {
                new_fd = i;
                break;
            }
        }

        if (new_fd < 0) {
            // Put child back and return EMFILE
            child->backlog_head = NULL;
            child->backlog_tail = NULL;
            listen_sock->backlog_len++;
            if (listen_sock->backlog_head) {
                child->backlog_head = listen_sock->backlog_head;
                listen_sock->backlog_head = child;
            } else {
                listen_sock->backlog_head = listen_sock->backlog_tail = child;
            }
            spin_unlock(&socket_lock);
            return (uint64_t)-EMFILE;
        }

        // Transfer ownership to new fd (initial ref_count=1 from socket allocation)
        child->state = UNIX_CONNECTED;

        proc->fd_table[new_fd].type = FD_SOCKET;
        proc->fd_table[new_fd].flags = O_RDWR;
        proc->fd_table[new_fd].sock = child;

        // Write peer address if requested
        if (addr && addrlen) {
            socklen_t alen = sizeof(struct sockaddr_un);
            struct sockaddr_un sa;
            __memset(&sa, 0, sizeof(sa));
            sa.sun_family = AF_UNIX;
            // Find peer's sun_path from peer socket
            struct unix_sock *peer_sock = NULL;
            if (child->peer >= 0 && child->peer < MAX_PROC) {
                proc_t *pp = &procs[child->peer];
                if (pp->pid == child->peer) {
                    for (int pfd = 0; pfd < MAX_FD; pfd++) {
                        if (pp->fd_table[pfd].type == FD_SOCKET &&
                            pp->fd_table[pfd].sock &&
                            pp->fd_table[pfd].sock->peer == proc->pid) {
                            peer_sock = pp->fd_table[pfd].sock;
                            break;
                        }
                    }
                }
            }
            if (peer_sock && peer_sock->sun_path[0]) {
                int pi = 0;
                while (peer_sock->sun_path[pi] && pi < 107) {
                    sa.sun_path[pi] = peer_sock->sun_path[pi];
                    pi++;
                }
                sa.sun_path[pi] = '\0';
            }
            // Copy to user
            socklen_t user_alen;
            __memcpy(&user_alen, addrlen, sizeof(socklen_t));
            if (user_alen > alen) user_alen = alen;
            __memcpy(addr, &sa, user_alen);
            __memcpy(addrlen, &alen, sizeof(socklen_t));
        }

        spin_unlock(&socket_lock);
        return (uint64_t)new_fd;
    }
}

uint64_t sys_connect(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t _u1, uint64_t _u2, uint64_t _u3) {
    int fd = (int)arg1;
    const struct sockaddr_un *addr = (const struct sockaddr_un *)arg2;
    socklen_t addrlen = (socklen_t)arg3;

    proc_t *proc = current_proc;

    if (fd < 0 || fd >= MAX_FD) return (uint64_t)-EBADF;
    if (proc->fd_table[fd].type != FD_SOCKET) return (uint64_t)-ENOTSOCK;

    // Validate addr
    uint64_t addr_ptr = (uint64_t)addr;
    if (!addr_ptr || addr_ptr >= 0xFFFFFFFF80000000ULL ||
        addr_ptr + addrlen > 0xFFFFFFFF80000000ULL || addrlen <= 0)
        return (uint64_t)-EFAULT;

    uint16_t sun_family;
    __memcpy(&sun_family, addr, sizeof(sun_family));
    if (sun_family != AF_UNIX) return (uint64_t)-EAFNOSUPPORT;

    char sun_path[108];
    __memset(sun_path, 0, sizeof(sun_path));
    size_t path_len = addrlen - sizeof(sun_family);
    if (path_len > 107) path_len = 107;
    if (path_len > 0)
        __memcpy(sun_path, (const char *)addr + sizeof(sun_family), path_len);
    sun_path[107] = '\0';

    if (sun_path[0] == '\0') return (uint64_t)-EINVAL;

    spin_lock(&socket_lock);

    // Look up listener
    struct unix_sock *listener = NULL;
    int ret = unix_bind_lookup(sun_path, &listener);
    if (ret != 0) {
        spin_unlock(&socket_lock);
        return (uint64_t)(-ret);
    }

    if (listener->state != UNIX_LISTEN) {
        spin_unlock(&socket_lock);
        return (uint64_t)-ECONNREFUSED;
    }

    if (listener->backlog_len >= listener->backlog_max) {
        spin_unlock(&socket_lock);
        return (uint64_t)-ECONNREFUSED;
    }

    // Create child socket for this connection
    struct unix_sock *child = unix_sock_alloc();
    if (!child) {
        spin_unlock(&socket_lock);
        return (uint64_t)-ENOMEM;
    }

    child->state = UNIX_CONNECTED;
    child->peer = proc->pid;

    // Update our socket to CONNECTED and set peer
    struct unix_sock *client_sock = proc->fd_table[fd].sock;
    if (!client_sock) {
        spin_unlock(&socket_lock);
        unix_sock_free(child);
        return (uint64_t)-EBADF;
    }
    client_sock->state = UNIX_CONNECTED;
    // Find the PID that owns this listener socket.
    pid_t listener_pid = -1;
    for (int i = 0; i < MAX_PROC; i++) {
        if (procs[i].pid >= 0) {
            for (int pf = 0; pf < MAX_FD; pf++) {
                if (procs[i].fd_table[pf].type == FD_SOCKET &&
                    procs[i].fd_table[pf].sock == listener) {
                    listener_pid = procs[i].pid;
                    goto found_listener;
                }
            }
        }
    }
found_listener:

    client_sock->peer = listener_pid;
    child->peer = proc->pid;

    // Enqueue child to listener's backlog
    child->backlog_head = NULL;
    child->backlog_tail = NULL;
    if (listener->backlog_tail) {
        listener->backlog_tail->backlog_head = child;  // use backlog_head as next pointer for backlog chain
        listener->backlog_tail = child;
    } else {
        listener->backlog_head = child;
        listener->backlog_tail = child;
    }
    listener->backlog_len++;

    // Wake listener's blocked_reader (accept)
    pid_t blocked_reader = listener->blocked_reader;
    spin_unlock(&socket_lock);

    if (blocked_reader >= 0) {
        wake_process(blocked_reader);
    }

    return 0;
}

uint64_t sys_socketpair(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t _u1, uint64_t _u2) {
    int domain = (int)arg1;
    int type = (int)arg2;
    int protocol = (int)arg3;
    int *sv = (int *)arg4;

    if (domain != AF_UNIX) return (uint64_t)-EAFNOSUPPORT;
    if (type != SOCK_STREAM) return (uint64_t)-EPROTONOSUPPORT;
    if (protocol != 0) return (uint64_t)-EPROTONOSUPPORT;

    // Validate user pointer
    uint64_t sv_ptr = (uint64_t)sv;
    if (!sv_ptr || sv_ptr >= 0xFFFFFFFF80000000ULL || sv_ptr + 2 * sizeof(int) > 0xFFFFFFFF80000000ULL)
        return (uint64_t)-EFAULT;

    proc_t *proc = current_proc;

    struct unix_sock *a = unix_sock_alloc();
    struct unix_sock *b = unix_sock_alloc();
    if (!a || !b) {
        if (a) unix_sock_free(a);
        if (b) unix_sock_free(b);
        return (uint64_t)-ENOMEM;
    }

    spin_lock(&socket_lock);

    int fd_a = -1, fd_b = -1;
    for (int i = 3; i < MAX_FD; i++) {
        if (proc->fd_table[i].type == FD_NONE) {
            if (fd_a < 0) fd_a = i;
            else if (fd_b < 0) { fd_b = i; break; }
        }
    }

    if (fd_a < 0 || fd_b < 0) {
        spin_unlock(&socket_lock);
        unix_sock_free(a);
        unix_sock_free(b);
        return (uint64_t)-EMFILE;
    }

    a->state = UNIX_CONNECTED;
    a->peer = proc->pid;  // will be corrected below
    b->state = UNIX_CONNECTED;
    b->peer = proc->pid;

    a->peer = proc->pid;  // peer PID is ourselves (both in same process)
    b->peer = proc->pid;

    proc->fd_table[fd_a].type = FD_SOCKET;
    proc->fd_table[fd_a].flags = O_RDWR;
    proc->fd_table[fd_a].sock = a;

    proc->fd_table[fd_b].type = FD_SOCKET;
    proc->fd_table[fd_b].flags = O_RDWR;
    proc->fd_table[fd_b].sock = b;

    spin_unlock(&socket_lock);

    // Write fd pair to user space
    sv[0] = fd_a;
    sv[1] = fd_b;

    return 0;
}

uint64_t sys_sendmsg(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t _u1, uint64_t _u2, uint64_t _u3) {
    int fd = (int)arg1;
    const struct msghdr *msg = (const struct msghdr *)arg2;
    int flags = (int)arg3;

    proc_t *proc = current_proc;

    if (fd < 0 || fd >= MAX_FD) return (uint64_t)-EBADF;
    if (proc->fd_table[fd].type != FD_SOCKET) return (uint64_t)-ENOTSOCK;

    // Validate msghdr pointer
    uint64_t msg_ptr = (uint64_t)msg;
    if (!msg_ptr || msg_ptr >= 0xFFFFFFFF80000000ULL ||
        msg_ptr + sizeof(struct msghdr) > 0xFFFFFFFF80000000ULL)
        return (uint64_t)-EFAULT;

    struct msghdr kmsg;
    __memcpy(&kmsg, msg, sizeof(struct msghdr));

    // Validate iov
    uint64_t iov_ptr = (uint64_t)kmsg.msg_iov;
    if (!iov_ptr || iov_ptr >= 0xFFFFFFFF80000000ULL ||
        iov_ptr + kmsg.msg_iovlen * sizeof(struct iovec) > 0xFFFFFFFF80000000ULL ||
        kmsg.msg_iovlen == 0 || kmsg.msg_iovlen > 1024)
        return (uint64_t)-EINVAL;

    // Copy iovec array to kernel
    struct iovec *kiov = (struct iovec *)kmalloc(kmsg.msg_iovlen * sizeof(struct iovec));
    if (!kiov) return (uint64_t)-ENOMEM;
    __memcpy(kiov, kmsg.msg_iov, kmsg.msg_iovlen * sizeof(struct iovec));

    // Validate each iov base pointer
    for (size_t i = 0; i < kmsg.msg_iovlen; i++) {
        if (kiov[i].iov_base) {
            uint64_t base = (uint64_t)kiov[i].iov_base;
            if (base >= 0xFFFFFFFF80000000ULL || base + kiov[i].iov_len > 0xFFFFFFFF80000000ULL) {
                kfree(kiov);
                return (uint64_t)-EFAULT;
            }
        }
    }

    // Copy control data
    void *kcontrol = NULL;
    size_t kcontrollen = 0;
    if (kmsg.msg_control && kmsg.msg_controllen > 0) {
        uint64_t ctrl_ptr = (uint64_t)kmsg.msg_control;
        if (ctrl_ptr >= 0xFFFFFFFF80000000ULL ||
            ctrl_ptr + kmsg.msg_controllen > 0xFFFFFFFF80000000ULL) {
            kfree(kiov);
            return (uint64_t)-EFAULT;
        }
        if (kmsg.msg_controllen > 4096) {
            kfree(kiov);
            return (uint64_t)-EINVAL;
        }
        kcontrol = kmalloc(kmsg.msg_controllen);
        if (!kcontrol) { kfree(kiov); return (uint64_t)-ENOMEM; }
        __memcpy(kcontrol, kmsg.msg_control, kmsg.msg_controllen);
        kcontrollen = kmsg.msg_controllen;
    }

    struct unix_sock *sock = proc->fd_table[fd].sock;
    if (!sock) { kfree(kiov); if (kcontrol) kfree(kcontrol); return (uint64_t)-EBADF; }

    int64_t ret = sock_sendmsg_internal(sock, kiov, kmsg.msg_iovlen,
                                         kcontrol, kcontrollen, flags);

    kfree(kiov);
    if (kcontrol) kfree(kcontrol);

    return (uint64_t)ret;
}

uint64_t sys_recvmsg(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t _u1, uint64_t _u2, uint64_t _u3) {
    int fd = (int)arg1;
    struct msghdr *msg = (struct msghdr *)arg2;
    int flags = (int)arg3;

    proc_t *proc = current_proc;

    if (fd < 0 || fd >= MAX_FD) return (uint64_t)-EBADF;
    if (proc->fd_table[fd].type != FD_SOCKET) return (uint64_t)-ENOTSOCK;

    uint64_t msg_ptr = (uint64_t)msg;
    if (!msg_ptr || msg_ptr >= 0xFFFFFFFF80000000ULL ||
        msg_ptr + sizeof(struct msghdr) > 0xFFFFFFFF80000000ULL)
        return (uint64_t)-EFAULT;

    struct msghdr kmsg;
    __memcpy(&kmsg, msg, sizeof(struct msghdr));

    // Validate iov
    uint64_t iov_ptr = (uint64_t)kmsg.msg_iov;
    if (!iov_ptr || iov_ptr >= 0xFFFFFFFF80000000ULL ||
        iov_ptr + kmsg.msg_iovlen * sizeof(struct iovec) > 0xFFFFFFFF80000000ULL ||
        kmsg.msg_iovlen == 0 || kmsg.msg_iovlen > 1024)
        return (uint64_t)-EINVAL;

    struct iovec *kiov = (struct iovec *)kmalloc(kmsg.msg_iovlen * sizeof(struct iovec));
    if (!kiov) return (uint64_t)-ENOMEM;
    __memcpy(kiov, kmsg.msg_iov, kmsg.msg_iovlen * sizeof(struct iovec));

    for (size_t i = 0; i < kmsg.msg_iovlen; i++) {
        if (kiov[i].iov_base) {
            uint64_t base = (uint64_t)kiov[i].iov_base;
            if (base >= 0xFFFFFFFF80000000ULL || base + kiov[i].iov_len > 0xFFFFFFFF80000000ULL) {
                kfree(kiov);
                return (uint64_t)-EFAULT;
            }
        }
    }

    // Control buffer for SCM_RIGHTS output
    void *kcontrol = NULL;
    size_t kcontrollen = 0;
    if (kmsg.msg_control && kmsg.msg_controllen > 0) {
        uint64_t ctrl_ptr = (uint64_t)kmsg.msg_control;
        if (ctrl_ptr >= 0xFFFFFFFF80000000ULL ||
            ctrl_ptr + kmsg.msg_controllen > 0xFFFFFFFF80000000ULL) {
            kfree(kiov);
            return (uint64_t)-EFAULT;
        }
        kcontrol = (void *)ctrl_ptr;  // write directly to user space (lazy install writes fds there)
        kcontrollen = kmsg.msg_controllen;
    }

    struct unix_sock *sock = proc->fd_table[fd].sock;
    if (!sock) { kfree(kiov); return (uint64_t)-EBADF; }

    int64_t ret = sock_recvmsg_internal(sock, kiov, kmsg.msg_iovlen,
                                         kcontrol, &kcontrollen, flags);

    // Update msg_controllen and msg_flags in user space
    // (kernel runs on user's CR3 during syscall, so direct access works)
    if (ret >= 0) {
        if (kmsg.msg_control && kmsg.msg_controllen > 0) {
            // struct msghdr: offset 40 = msg_controllen (size_t)
            *(size_t *)((char *)msg + 40) = kcontrollen;
        }
    }

    kfree(kiov);
    return (uint64_t)ret;
}

uint64_t sys_shutdown(uint64_t arg1, uint64_t arg2, uint64_t _u1, uint64_t _u2, uint64_t _u3, uint64_t _u4) {
    int fd = (int)arg1;
    int how = (int)arg2;

    proc_t *proc = current_proc;

    if (fd < 0 || fd >= MAX_FD) return (uint64_t)-EBADF;
    if (proc->fd_table[fd].type != FD_SOCKET) return (uint64_t)-ENOTSOCK;

    struct unix_sock *sock = proc->fd_table[fd].sock;
    if (!sock) return (uint64_t)-EBADF;

    if (how < 0 || how > 2) return (uint64_t)-EINVAL;

    spin_lock(&socket_lock);

    if (how == SHUT_RD || how == SHUT_RDWR) {
        sock->shutdown_read = 1;
        // Free recv queue
        while (sock->recv_queue_head) {
            struct sk_buff *skb = skb_dequeue(sock);
            skb_free(skb);
        }
    }
    if (how == SHUT_WR || how == SHUT_RDWR) {
        sock->shutdown_write = 1;
    }

    pid_t reader = sock->blocked_reader;
    pid_t writer = sock->blocked_writer;
    sock->blocked_reader = -1;
    sock->blocked_writer = -1;

    spin_unlock(&socket_lock);

    if (reader >= 0) wake_process(reader);
    if (writer >= 0) wake_process(writer);

    return 0;
}

uint64_t sys_poll(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t _u1, uint64_t _u2, uint64_t _u3) {
    struct pollfd *fds = (struct pollfd *)arg1;
    nfds_t nfds = (nfds_t)arg2;
    int timeout_ms = (int)arg3;

    proc_t *proc = current_proc;

    // Validate user pointer
    uint64_t fds_ptr = (uint64_t)fds;
    if (!fds_ptr || fds_ptr >= 0xFFFFFFFF80000000ULL ||
        fds_ptr + nfds * sizeof(struct pollfd) > 0xFFFFFFFF80000000ULL)
        return (uint64_t)-EFAULT;

    // Copy pollfd array to kernel
    struct pollfd *kfds = (struct pollfd *)kmalloc(nfds * sizeof(struct pollfd));
    if (!kfds) return (uint64_t)-ENOMEM;
    __memcpy(kfds, fds, nfds * sizeof(struct pollfd));

    uint64_t deadline = 0;
    if (timeout_ms > 0) {
        deadline = sched_clock() + (uint64_t)timeout_ms * 1000000ULL;
    }

    while (1) {
        int ready = 0;

        // Check each fd
        for (nfds_t i = 0; i < nfds; i++) {
            kfds[i].revents = 0;
            int pfd = kfds[i].fd;

            if (pfd < 0 || pfd >= MAX_FD) {
                kfds[i].revents = POLLERR;
                ready++;
                continue;
            }

            struct file *f = &proc->fd_table[pfd];
            if (f->type == FD_NONE) {
                kfds[i].revents = POLLERR;
                ready++;
                continue;
            }

            if (f->type == FD_PIPE) {
                struct pipe *p = f->pipe;
                if (p) {
                    // POLLIN: data available or EOF (no writers)
                    if (p->head != p->tail || p->ref_count <= 1) {
                        if (kfds[i].events & POLLIN) kfds[i].revents |= POLLIN;
                        ready++;
                    }
                    // POLLOUT: space available or peer closed
                    if ((p->head + 1) % PIPE_BUF_SIZE != p->tail || p->ref_count <= 1) {
                        if (kfds[i].events & POLLOUT) kfds[i].revents |= POLLOUT;
                        ready++;
                    }
                }
            } else if (f->type == FD_SOCKET) {
                struct unix_sock *sock = f->sock;
                if (sock) {
                    spin_lock(&socket_lock);

                    // POLLIN: data available or shutdown_read
                    if (sock->recv_queue_head || sock->shutdown_read) {
                        if (kfds[i].events & POLLIN) kfds[i].revents |= POLLIN;
                        ready++;
                    }

                    // POLLOUT: not shutdown_write
                    if (!sock->shutdown_write && sock->state == UNIX_CONNECTED) {
                        if (kfds[i].events & POLLOUT) kfds[i].revents |= POLLOUT;
                        ready++;
                    }

                    // POLLHUP: peer gone
                    if (sock->state == UNIX_CLOSED ||
                        (sock->peer >= 0 && sock->peer < MAX_PROC &&
                         procs[sock->peer].pid != sock->peer)) {
                        kfds[i].revents |= POLLHUP;
                        ready++;
                    }

                    // For LISTEN socket: POLLIN = has pending connections
                    if (sock->state == UNIX_LISTEN && sock->backlog_len > 0) {
                        if (kfds[i].events & POLLIN) kfds[i].revents |= POLLIN;
                        ready++;
                    }

                    spin_unlock(&socket_lock);
                }
            } else if (f->type == FD_FILE) {
                // FD_FILE: always writable (buffered), POLLIN if not at EOF
                if (kfds[i].events & POLLOUT) kfds[i].revents |= POLLOUT;
                ready++;
                // For POLLIN: check if offset < file_size
                if ((kfds[i].events & POLLIN) &&
                    f->file_data.offset < f->file_data.file_size) {
                    kfds[i].revents |= POLLIN;
                }
            } else {
                // FD_DEV, FD_SHM: always ready
                if (kfds[i].events & POLLIN) kfds[i].revents |= POLLIN;
                if (kfds[i].events & POLLOUT) kfds[i].revents |= POLLOUT;
                ready++;
            }
        }

        if (ready > 0) {
            // Copy results back to user
            __memcpy(fds, kfds, nfds * sizeof(struct pollfd));
            kfree(kfds);
            return (uint64_t)ready;
        }

        if (timeout_ms == 0) {
            __memcpy(fds, kfds, nfds * sizeof(struct pollfd));
            kfree(kfds);
            return 0;
        }

        // Block on WAIT_POLL
        if (deadline > 0) {
            uint64_t now = sched_clock();
            if (now >= deadline) {
                __memset(kfds, 0, nfds * sizeof(struct pollfd));
                __memcpy(fds, kfds, nfds * sizeof(struct pollfd));
                kfree(kfds);
                return 0;  // timeout
            }
            proc->wait_deadline = deadline;
            spin_lock(&cpu_locals[proc->assigned_cpu].scheduler_lock);
            timer_queue_insert(proc->assigned_cpu, proc);
            spin_unlock(&cpu_locals[proc->assigned_cpu].scheduler_lock);
        } else {
            proc->wait_deadline = 0;
        }

        proc->state = BLOCKED;
        proc->wait_event = WAIT_POLL;
        proc->wait_timed_out = 0;
        schedule();

        if (proc->wait_timed_out && timeout_ms > 0) {
            __memset(kfds, 0, nfds * sizeof(struct pollfd));
            __memcpy(fds, kfds, nfds * sizeof(struct pollfd));
            kfree(kfds);
            return 0;  // timeout
        }

        // Woken up — re-check all fds
    }
}

// ===================== sock_write / sock_read (for sys_write/sys_read FD_SOCKET dispatch) =====================

int64_t sock_write(struct unix_sock *sock, const void *buf, size_t len) {
    struct iovec iov;
    iov.iov_base = (void *)buf;
    iov.iov_len = len;
    return sock_sendmsg_internal(sock, &iov, 1, NULL, 0, 0);
}

int64_t sock_read(struct unix_sock *sock, void *buf, size_t len) {
    struct iovec iov;
    iov.iov_base = buf;
    iov.iov_len = len;
    return sock_recvmsg_internal(sock, &iov, 1, NULL, NULL, 0);
}

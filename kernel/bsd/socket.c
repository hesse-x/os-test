#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "kernel/bsd/socket.h"
#include "kernel/xcore/sched.h"
#include "kernel/bsd/types.h"
#include "kernel/bsd/proc.h"
#include "kernel/bsd/devtmpfs.h"
#include "kernel/xcore/trap.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/spinlock.h"
#include "common/errno.h"
#include "kernel/xcore/mem/kasan.h"
#include "kernel/bsd/pty.h"
#include "arch/x64/utils.h"
#include "arch/x64/apic.h"
#include "kernel/xcore/rcu.h"

// ===================== Global socket lock =====================
spinlock_t socket_lock = SPINLOCK_INIT;

// ===================== Bind name space =====================
static struct unix_bind_entry *unix_bind_table[UNIX_HASH_SIZE];

static uint32_t unix_hash(const char *path) {
    uint32_t h = 0;
    for (int i = 0; path[i] && i < 108; i++) {
        h = h * 31 + (uint8_t)path[i];
    }
    return h % UNIX_HASH_SIZE;
}

int unix_bind_lookup(const char *sun_path, struct unix_sock **out, pid_t *owner_pid) {
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
                *owner_pid = e->owner_pid;
                return 0;
            }
            return -ENOENT;  // exists but not listening
        }
    }
    return -ENOENT;
}

int unix_bind_register(const char *sun_path, struct unix_sock *sock, pid_t owner_pid) {
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
    entry->owner_pid = owner_pid;
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
    sock->peer_sock = NULL;
    refcount_set(&sock->u_count, 1);
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
            xtask_t *peer = &tasks[bp->peer];
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
    if (sock) refcount_inc(&sock->u_count);
}

void unix_sock_release(struct unix_sock *sock) {
    if (!sock) return;
    if (refcount_dec_and_test(&sock->u_count)) {
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
    // Check shutdown_write on our socket
    if (sock->shutdown_write) return -EPIPE;
    // Check peer shutdown_read — sending to a socket whose read side is shut down should EPIPE
    if (sock->peer_sock && sock->peer_sock->shutdown_read) return -EPIPE;

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
            copy_from_user(skb->data + offset, (const void __user *)iov[i].iov_base, iov[i].iov_len);
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
                    rcu_read_lock();
                    for (int i = 0; i < num_fds; i++) {
                        if (fds[i] < 0 || fds[i] >= MAX_FD) {
                            rcu_read_unlock();
                            skb_free(skb);
                            return -EBADF;
                        }
                        struct file *tf = fd_lookup(current_proc->files, fds[i]);
                        if (!tf) {
                            rcu_read_unlock();
                            skb_free(skb);
                            return -EBADF;
                        }
                        skb->fds[skb->num_fds++] = fds[i];
                    }
                    rcu_read_unlock();
                }
            } else {
                skb_free(skb);
                return -EINVAL;
            }
            cmsg_ptr += aligned_len;
        }
    }

    // Find peer socket via direct pointer (socketpair) or PID-based lookup (connect)
    struct unix_sock *peer_sock = sock->peer_sock;
    if (!peer_sock) {
        // peer_sock is always set during connect/socketpair; this should not be reached
        WARN_ON(!peer_sock);
        skb_free(skb);
        return -EPIPE;
    }

    // Check peer shutdown (under socket_lock)
    spin_lock(&socket_lock);
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
            // 1) Our own shutdown_read
            if (sock->shutdown_read) {
                spin_unlock(&socket_lock);
                return 0;  // EOF
            }
            // 2) Peer shutdown_write (we will never receive more data)
            if (sock->peer_sock && sock->peer_sock->shutdown_write) {
                spin_unlock(&socket_lock);
                return 0;  // EOF — peer shut down write side
            }
            // 3) Socket closed or peer zombie
            if (sock->state == UNIX_CLOSED ||
                (sock->peer >= 0 && tasks[sock->peer].pid == sock->peer &&
                 tasks[sock->peer].state == ZOMBIE)) {
                // Check if peer has closed
                bool peer_closed = true;
                if (sock->peer_sock) {
                    peer_closed = (sock->peer_sock->state != UNIX_CONNECTED);
                } else {
                    spin_unlock(&socket_lock);
                    // peer_sock is always set during connect/socketpair; this should not be reached
                    WARN_ON(!sock->peer_sock);
                    // without peer_sock, assume peer closed
                    return 0;  // EOF
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
            sock->blocked_reader = current_task->pid;
            xtask_t *proc = current_task;
            proc->state = BLOCKED;
            proc->wait_event = WAIT_POLL;
            proc->wait_deadline = sched_clock() + 30000000000ULL;  // 30s default timeout
            spin_unlock(&socket_lock);
            int cpu = proc->assigned_cpu;
            uint64_t rflags;
            spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &rflags);
            timer_queue_insert(cpu, proc);
            spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, rflags);
            schedule();
            // Timeout check
            if (proc->wait_timed_out) {
                spin_lock(&socket_lock);
                sock->blocked_reader = -1;
                spin_unlock(&socket_lock);
                return -ETIMEDOUT;
            }
            // EINTR check
            {
                uint64_t pend = __atomic_load_n(&proc->proc->sig_pending, __ATOMIC_ACQUIRE);
                uint64_t deliv = pend & ~proc->proc->sig_blocked;
                deliv |= (pend & ((1ULL << SIGKILL) | (1ULL << SIGSTOP)));
                if (deliv) {
                    spin_lock(&socket_lock);
                    sock->blocked_reader = -1;
                    spin_unlock(&socket_lock);
                    return (int64_t)-EINTR;
                }
            }
            spin_lock(&socket_lock);
            sock->blocked_reader = -1;
            // Re-try after wake
            spin_unlock(&socket_lock);
            continue;
        }

        ; // We have data. Calculate how much to read.
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
                copy_to_user((void __user *)iov[i].iov_base, skb->data + data_offset, copy);
                data_offset += copy;
                remaining -= copy;
            }
        }

        skb->consumed += to_read;
        bool skb_consumed = (skb->consumed >= skb->len);

        // Extract SCM_RIGHTS data from skb before releasing socket_lock
        int num_fds_to_install = 0;
        int orig_fds[SCM_MAX_FD];
        pid_t sender_pid = sock->peer;
        if (skb->num_fds > 0 && skb_consumed) {
            for (int i = 0; i < skb->num_fds && num_fds_to_install < SCM_MAX_FD; i++) {
                orig_fds[num_fds_to_install++] = skb->fds[i];
            }
        }

        // Remove consumed skb and release socket_lock
        bool do_scm = (num_fds_to_install > 0);
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

        // Process SCM_RIGHTS — sequential locks, no nesting
        int num_fds_installed = 0;
        int installed_fds[SCM_MAX_FD];
        if (do_scm) {
            xtask_t *proc = current_task;
            for (int i = 0; i < num_fds_to_install && num_fds_installed < SCM_MAX_FD; i++) {
                int orig_fd = orig_fds[i];
                // Step 1: RCU read — validate + file_get (atomic verify+hold, no TOCTOU)
                struct file *src = NULL;
                if (sender_pid >= 0 && sender_pid < MAX_PROC) {
                    xtask_t *sender = &tasks[sender_pid];
                    if (sender->pid == sender_pid && sender->mm && sender->proc->files && orig_fd >= 0 && orig_fd < MAX_FD) {
                        rcu_read_lock();
                        src = fd_lookup(sender->proc->files, orig_fd);
                        if (src) file_get(src);   // hold ref, sender close won't free
                        rcu_read_unlock();
                    }
                }
                if (!src) continue;

                // Step 2: receiver_fd_lock — find free slot + install pointer
                spinlock_t *fdlk = &proc->proc->files->fd_lock;
                spin_lock(fdlk);
                int new_fd = alloc_fd(proc->proc->files, 0);
                if (new_fd < 0) {
                    spin_unlock(fdlk);
                    file_put(src);
                    break;  // no free fd slots
                }
                fd_install(proc->proc->files, new_fd, src);  // install pointer directly, no extra bump needed
                if (src->type == FD_TTY) pty_dup_file(src);
                spin_unlock(fdlk);

                installed_fds[num_fds_installed++] = new_fd;
            }
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
    struct unix_sock *peer_s = sock->peer_sock;
    pid_t peer_pid = sock->peer;
    pid_t peer_reader = -1, peer_writer = -1;
    if (peer_s) {
        peer_reader = peer_s->blocked_reader;
        peer_writer = peer_s->blocked_writer;
        peer_s->blocked_reader = -1;
        peer_s->blocked_writer = -1;
    }
    sock->blocked_reader = -1;
    sock->blocked_writer = -1;

    spin_unlock(&socket_lock);

    // Wake our blocked reader/writer outside lock
    if (reader >= 0) wake_process(reader);
    if (writer >= 0) wake_process(writer);

    // Wake peer's blocked reader/writer so it can detect EOF/EPIPE
    if (peer_reader >= 0) wake_process(peer_reader);
    if (peer_writer >= 0) wake_process(peer_writer);
    // Also wake peer process itself (PID-based fallback for cross-process)
    if (!peer_s && peer_pid >= 0) wake_process(peer_pid);

    // Release reference (actual free when ref_count hits 0)
    unix_sock_release(sock);
}

// ===================== Syscall implementations =====================

int64_t sys_socket(int64_t arg1, int64_t arg2, int64_t arg3, int64_t _u1, int64_t _u2, int64_t _u3) {
    int domain = (int)arg1;
    int type = (int)arg2;
    int protocol = (int)arg3;

    // Only AF_UNIX SOCK_STREAM supported
    if (domain != AF_UNIX) return (int64_t)-EAFNOSUPPORT;
    if (type != SOCK_STREAM) return (int64_t)-EPROTONOSUPPORT;
    if (protocol != 0) return (int64_t)-EPROTONOSUPPORT;

    struct unix_sock *sock = unix_sock_alloc();
    if (!sock) return (int64_t)-ENOMEM;

    xtask_t *proc = current_task;

    sock->state = UNIX_FREE;

    spinlock_t *fdlk = &proc->proc->files->fd_lock;
    spin_lock(fdlk);

    // Find free fd slot
    int fd = alloc_fd(proc->proc->files, 3);
    if (fd < 0) {
        spin_unlock(fdlk);
        unix_sock_release(sock);
        return (int64_t)-EMFILE;
    }

    struct file *f = (struct file *)kmalloc(sizeof(struct file));
    if (!f) {
        spin_unlock(fdlk);
        unix_sock_release(sock);
        return (int64_t)-ENOMEM;
    }
    __memset(f, 0, sizeof(*f));
    refcount_set(&f->f_count, 1);
    f->type = FD_SOCKET;
    f->flags = O_RDWR;
    f->sock = sock;
    fd_install(proc->proc->files, fd, f);

    spin_unlock(fdlk);

    return (int64_t)fd;
}

int64_t sys_bind(int64_t arg1, int64_t arg2, int64_t arg3, int64_t _u1, int64_t _u2, int64_t _u3) {
    int fd = (int)arg1;
    const struct sockaddr_un __user *addr = (const struct sockaddr_un __user *)arg2;
    socklen_t addrlen = (socklen_t)arg3;

    xtask_t *proc = current_task;

    // Validate fd
    if (fd < 0 || fd >= MAX_FD) return (int64_t)-EBADF;
    rcu_read_lock();
    struct file *bf = fd_lookup(proc->proc->files, fd);
    if (!bf || bf->type != FD_SOCKET) { rcu_read_unlock(); return (int64_t)-ENOTSOCK; }
    file_get(bf);
    rcu_read_unlock();

    // Validate addr
    uint64_t addr_ptr = (int64_t)addr;
    if (!addr_ptr || addr_ptr >= 0xFFFFFFFF80000000ULL ||
        addr_ptr + addrlen > 0xFFFFFFFF80000000ULL || addrlen <= 0)
        { file_put(bf); return (int64_t)-EFAULT; }

    // Read sun_family
    uint16_t sun_family;
    copy_from_user(&sun_family, addr, sizeof(sun_family));
    if (sun_family != AF_UNIX) { file_put(bf); return (int64_t)-EAFNOSUPPORT; }

    // Read sun_path (max 108 bytes)
    char sun_path[108];
    __memset(sun_path, 0, sizeof(sun_path));
    size_t path_len = addrlen - sizeof(sun_family);
    if (path_len > 107) path_len = 107;
    if (path_len > 0)
        copy_from_user(sun_path, (const char __user *)addr + sizeof(sun_family), path_len);
    sun_path[107] = '\0';

    if (sun_path[0] == '\0') { file_put(bf); return (int64_t)-EINVAL; }  // empty path

    struct unix_sock *sock = bf->sock;
    if (!sock) { file_put(bf); return (int64_t)-EBADF; }

    spin_lock(&socket_lock);

    if (sock->state != UNIX_FREE) {
        spin_unlock(&socket_lock);
        file_put(bf);
        return (int64_t)-EINVAL;
    }

    // Register in name space
    int ret = unix_bind_register(sun_path, sock, current_task->pid);
    if (ret != 0) {
        spin_unlock(&socket_lock);
        file_put(bf);
        return (int64_t)(-ret);
    }

    // Save sun_path in sock
    int i = 0;
    while (sun_path[i] && i < 107) {
        sock->sun_path[i] = sun_path[i];
        i++;
    }
    sock->sun_path[i] = '\0';

    spin_unlock(&socket_lock);

    file_put(bf);
    return 0;
}

int64_t sys_listen(int64_t arg1, int64_t arg2, int64_t _u1, int64_t _u2, int64_t _u3, int64_t _u4) {
    int fd = (int)arg1;
    int backlog = (int)arg2;

    xtask_t *proc = current_task;

    if (fd < 0 || fd >= MAX_FD) return (int64_t)-EBADF;
    rcu_read_lock();
    struct file *lf = fd_lookup(proc->proc->files, fd);
    if (!lf || lf->type != FD_SOCKET) { rcu_read_unlock(); return (int64_t)-ENOTSOCK; }
    file_get(lf);
    rcu_read_unlock();

    struct unix_sock *sock = lf->sock;
    if (!sock) { file_put(lf); return (int64_t)-EBADF; }

    if (backlog <= 0) backlog = 1;
    if (backlog > UNIX_MAX_BACKLOG) backlog = UNIX_MAX_BACKLOG;

    spin_lock(&socket_lock);
    sock->state = UNIX_LISTEN;
    sock->backlog_max = backlog;
    spin_unlock(&socket_lock);

    file_put(lf);
    return 0;
}

int64_t sys_accept(int64_t arg1, int64_t arg2, int64_t arg3, int64_t _u1, int64_t _u2, int64_t _u3) {
    int fd = (int)arg1;
    struct sockaddr_un __user *addr = (struct sockaddr_un __user *)arg2;
    socklen_t __user *addrlen = (socklen_t __user *)arg3;

    xtask_t *proc = current_task;

    if (fd < 0 || fd >= MAX_FD) return (int64_t)-EBADF;
    rcu_read_lock();
    struct file *af = fd_lookup(proc->proc->files, fd);
    if (!af || af->type != FD_SOCKET) { rcu_read_unlock(); return (int64_t)-ENOTSOCK; }
    file_get(af);
    rcu_read_unlock();

    struct unix_sock *listen_sock = af->sock;
    if (!listen_sock) { file_put(af); return (int64_t)-EBADF; }

    // Validate addr pointers if non-null
    if (addr) {
        uint64_t aptr = (int64_t)addr;
        if (aptr >= 0xFFFFFFFF80000000ULL) { file_put(af); return (int64_t)-EFAULT; }
    }
    if (addrlen) {
        uint64_t alptr = (int64_t)addrlen;
        if (alptr >= 0xFFFFFFFF80000000ULL || alptr + sizeof(socklen_t) > 0xFFFFFFFF80000000ULL)
            { file_put(af); return (int64_t)-EFAULT; }
    }

    int64_t ret;
    while (1) {
        spin_lock(&socket_lock);

        if (listen_sock->state != UNIX_LISTEN) {
            spin_unlock(&socket_lock);
            ret = -EINVAL;
            goto out;
        }

        struct unix_sock *child = listen_sock->backlog_head;

        if (!child) {
            // No pending connections: block
            listen_sock->blocked_reader = proc->pid;
            proc->state = BLOCKED;
            proc->wait_event = WAIT_POLL;
            proc->wait_deadline = sched_clock() + 30000000000ULL;  // 30 second timeout
            spin_unlock(&socket_lock);
            int cpu = proc->assigned_cpu;
            uint64_t rflags;
            spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &rflags);
            timer_queue_insert(cpu, proc);
            spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, rflags);
            schedule();
            // Timeout check
            if (proc->wait_timed_out) {
                spin_lock(&socket_lock);
                listen_sock->blocked_reader = -1;
                spin_unlock(&socket_lock);
                ret = -ETIMEDOUT;
                goto out;
            }
            // EINTR check
            {
                uint64_t pend = __atomic_load_n(&proc->proc->sig_pending, __ATOMIC_ACQUIRE);
                uint64_t deliv = pend & ~proc->proc->sig_blocked;
                deliv |= (pend & ((1ULL << SIGKILL) | (1ULL << SIGSTOP)));
                if (deliv) {
                    spin_lock(&socket_lock);
                    listen_sock->blocked_reader = -1;
                    spin_unlock(&socket_lock);
                    ret = -EINTR;
                    goto out;
                }
            }
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
        spin_unlock(&socket_lock);

        // Allocate new fd for this child socket (fd_lock only, no socket_lock nesting)
        spinlock_t *fdlk = &proc->proc->files->fd_lock;
        spin_lock(fdlk);
        int new_fd = alloc_fd(proc->proc->files, 3);

        if (new_fd < 0) {
            spin_unlock(fdlk);
            // Put child back into backlog
            spin_lock(&socket_lock);
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
            ret = -EMFILE;
            goto out;
        }

        // Transfer ownership to new fd (initial ref_count=1 from socket allocation)
        child->state = UNIX_CONNECTED;

        struct file *f = (struct file *)kmalloc(sizeof(struct file));
        if (!f) {
            fd_uninstall(proc->proc->files, new_fd);
            spin_unlock(fdlk);
            // Put child back into backlog
            spin_lock(&socket_lock);
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
            ret = -ENOMEM;
            goto out;
        }
        __memset(f, 0, sizeof(*f));
        refcount_set(&f->f_count, 1);
        f->type = FD_SOCKET;
        f->flags = O_RDWR;
        f->sock = child;
        fd_install(proc->proc->files, new_fd, f);

        spin_unlock(fdlk);
        if (addr && addrlen) {
            socklen_t alen = sizeof(struct sockaddr_un);
            struct sockaddr_un sa;
            __memset(&sa, 0, sizeof(sa));
            sa.sun_family = AF_UNIX;
            // Find peer's sun_path from peer socket (direct pointer, set during connect)
            struct unix_sock *peer_sock = child->peer_sock;
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
            copy_from_user(&user_alen, addrlen, sizeof(socklen_t));
            if (user_alen > alen) user_alen = alen;
            copy_to_user(addr, &sa, user_alen);
            copy_to_user(addrlen, &alen, sizeof(socklen_t));
        }

        ret = (int64_t)new_fd;
        goto out;
    }
out:
    file_put(af);
    return ret;
}

int64_t sys_connect(int64_t arg1, int64_t arg2, int64_t arg3, int64_t _u1, int64_t _u2, int64_t _u3) {
    int fd = (int)arg1;
    const struct sockaddr_un __user *addr = (const struct sockaddr_un __user *)arg2;
    socklen_t addrlen = (socklen_t)arg3;

    xtask_t *proc = current_task;

    if (fd < 0 || fd >= MAX_FD) return (int64_t)-EBADF;
    rcu_read_lock();
    struct file *cf = fd_lookup(proc->proc->files, fd);
    if (!cf || cf->type != FD_SOCKET) { rcu_read_unlock(); return (int64_t)-ENOTSOCK; }
    file_get(cf);
    rcu_read_unlock();

    // Validate addr
    uint64_t addr_ptr = (int64_t)addr;
    if (!addr_ptr || addr_ptr >= 0xFFFFFFFF80000000ULL ||
        addr_ptr + addrlen > 0xFFFFFFFF80000000ULL || addrlen <= 0)
        { file_put(cf); return (int64_t)-EFAULT; }

    uint16_t sun_family;
    copy_from_user(&sun_family, addr, sizeof(sun_family));
    if (sun_family != AF_UNIX) { file_put(cf); return (int64_t)-EAFNOSUPPORT; }

    char sun_path[108];
    __memset(sun_path, 0, sizeof(sun_path));
    size_t path_len = addrlen - sizeof(sun_family);
    if (path_len > 107) path_len = 107;
    if (path_len > 0)
        copy_from_user(sun_path, (const char __user *)addr + sizeof(sun_family), path_len);
    sun_path[107] = '\0';

    if (sun_path[0] == '\0') { file_put(cf); return (int64_t)-EINVAL; }

    spin_lock(&socket_lock);

    // Look up listener and owner PID via sun_path hash
    struct unix_sock *listener = NULL;
    pid_t listener_pid = -1;
    int ret = unix_bind_lookup(sun_path, &listener, &listener_pid);
    if (ret != 0) {
        spin_unlock(&socket_lock);
        file_put(cf);
        return (int64_t)(-ret);
    }

    if (listener->state != UNIX_LISTEN) {
        spin_unlock(&socket_lock);
        file_put(cf);
        return (int64_t)-ECONNREFUSED;
    }

    if (listener->backlog_len >= listener->backlog_max) {
        spin_unlock(&socket_lock);
        file_put(cf);
        return (int64_t)-ECONNREFUSED;
    }

    // Create child socket for this connection
    struct unix_sock *child = unix_sock_alloc();
    if (!child) {
        spin_unlock(&socket_lock);
        file_put(cf);
        return (int64_t)-ENOMEM;
    }

    child->state = UNIX_CONNECTED;
    child->peer = proc->pid;

    // Update our socket to CONNECTED and set peer
    struct unix_sock *client_sock = cf->sock;
    if (!client_sock) {
        spin_unlock(&socket_lock);
        unix_sock_free(child);
        file_put(cf);
        return (int64_t)-EBADF;
    }
    client_sock->state = UNIX_CONNECTED;

    client_sock->peer = listener_pid;
    client_sock->peer_sock = child;  // client's peer is the child (server-side)
    child->peer = proc->pid;
    child->peer_sock = client_sock;  // child's peer is the client

    // Enqueue child to listener's backlog
    child->backlog_head = NULL;
    child->backlog_tail = NULL;
    if (listener->backlog_tail) {
        listener->backlog_tail->backlog_head = child;
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

    file_put(cf);
    return 0;
}

int64_t sys_socketpair(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4, int64_t _u1, int64_t _u2) {
    int domain = (int)arg1;
    int type = (int)arg2;
    int protocol = (int)arg3;
    int __user *sv = (int __user *)arg4;

    if (domain != AF_UNIX) return (int64_t)-EAFNOSUPPORT;
    if (type != SOCK_STREAM) return (int64_t)-EPROTONOSUPPORT;
    if (protocol != 0) return (int64_t)-EPROTONOSUPPORT;

    // Validate user pointer
    uint64_t sv_ptr = (int64_t)sv;
    if (!sv_ptr || sv_ptr >= 0xFFFFFFFF80000000ULL || sv_ptr + 2 * sizeof(int) > 0xFFFFFFFF80000000ULL)
        return (int64_t)-EFAULT;

    xtask_t *proc = current_task;

    struct unix_sock *a = unix_sock_alloc();
    struct unix_sock *b = unix_sock_alloc();
    if (!a || !b) {
        if (a) unix_sock_free(a);
        if (b) unix_sock_free(b);
        return (int64_t)-ENOMEM;
    }

    a->state = UNIX_CONNECTED;
    a->peer = proc->pid;
    a->peer_sock = b;
    b->state = UNIX_CONNECTED;
    b->peer = proc->pid;
    b->peer_sock = a;

    spinlock_t *fdlk = &proc->proc->files->fd_lock;
    spin_lock(fdlk);

    int fd_a = alloc_fd(proc->proc->files, 3);
    int fd_b = -1;
    if (fd_a >= 0) {
        fd_b = alloc_fd(proc->proc->files, fd_a + 1);
    }

    if (fd_a < 0 || fd_b < 0) {
        spin_unlock(fdlk);
        unix_sock_free(a);
        unix_sock_free(b);
        return (int64_t)-EMFILE;
    }

    a->state = UNIX_CONNECTED;
    a->peer = proc->pid;
    a->peer_sock = b;
    b->state = UNIX_CONNECTED;
    b->peer = proc->pid;
    b->peer_sock = a;

    struct file *fa = (struct file *)kmalloc(sizeof(struct file));
    struct file *fb = (struct file *)kmalloc(sizeof(struct file));
    if (!fa || !fb) {
        if (fa) kfree(fa);
        if (fb) kfree(fb);
        fd_uninstall(proc->proc->files, fd_a);
        fd_uninstall(proc->proc->files, fd_b);
        spin_unlock(fdlk);
        unix_sock_free(a);
        unix_sock_free(b);
        return (int64_t)-ENOMEM;
    }

    __memset(fa, 0, sizeof(*fa));
    refcount_set(&fa->f_count, 1);
    fa->type = FD_SOCKET;
    fa->flags = O_RDWR;
    fa->sock = a;
    fd_install(proc->proc->files, fd_a, fa);

    __memset(fb, 0, sizeof(*fb));
    refcount_set(&fb->f_count, 1);
    fb->type = FD_SOCKET;
    fb->flags = O_RDWR;
    fb->sock = b;
    fd_install(proc->proc->files, fd_b, fb);

    spin_unlock(fdlk);

    // Write fd pair to user space
    int fds[2] = { fd_a, fd_b };
    if (copy_to_user(sv, fds, sizeof(fds)))
        return (int64_t)-EFAULT;

    return 0;
}

int64_t sys_sendmsg(int64_t arg1, int64_t arg2, int64_t arg3, int64_t _u1, int64_t _u2, int64_t _u3) {
    int fd = (int)arg1;
    const struct msghdr __user *msg = (const struct msghdr __user *)arg2;
    int flags = (int)arg3;

    xtask_t *proc = current_task;

    if (fd < 0 || fd >= MAX_FD) return (int64_t)-EBADF;
    rcu_read_lock();
    struct file *sf = fd_lookup(proc->proc->files, fd);
    if (!sf || sf->type != FD_SOCKET) { rcu_read_unlock(); return (int64_t)-ENOTSOCK; }
    file_get(sf);
    rcu_read_unlock();

    // Validate msghdr pointer
    uint64_t msg_ptr = (int64_t)msg;
    if (!msg_ptr || msg_ptr >= 0xFFFFFFFF80000000ULL ||
        msg_ptr + sizeof(struct msghdr) > 0xFFFFFFFF80000000ULL)
        { file_put(sf); return (int64_t)-EFAULT; }

    struct msghdr kmsg;
    copy_from_user(&kmsg, msg, sizeof(struct msghdr));

    // Validate iov
    uint64_t iov_ptr = (int64_t)kmsg.msg_iov;
    if (!iov_ptr || iov_ptr >= 0xFFFFFFFF80000000ULL ||
        iov_ptr + kmsg.msg_iovlen * sizeof(struct iovec) > 0xFFFFFFFF80000000ULL ||
        kmsg.msg_iovlen == 0 || kmsg.msg_iovlen > 1024)
        { file_put(sf); return (int64_t)-EINVAL; }

    // Copy iovec array to kernel
    struct iovec *kiov = (struct iovec *)kmalloc(kmsg.msg_iovlen * sizeof(struct iovec));
    if (!kiov) { file_put(sf); return (int64_t)-ENOMEM; }
    copy_from_user(kiov, (const void __user *)kmsg.msg_iov, kmsg.msg_iovlen * sizeof(struct iovec));

    // Validate each iov base pointer
    for (size_t i = 0; i < kmsg.msg_iovlen; i++) {
        if (kiov[i].iov_base) {
            uint64_t base = (int64_t)kiov[i].iov_base;
            if (base >= 0xFFFFFFFF80000000ULL || base + kiov[i].iov_len > 0xFFFFFFFF80000000ULL) {
                kfree(kiov);
                file_put(sf);
                return (int64_t)-EFAULT;
            }
        }
    }

    // Copy control data
    void *kcontrol = NULL;
    size_t kcontrollen = 0;
    if (kmsg.msg_control && kmsg.msg_controllen > 0) {
        uint64_t ctrl_ptr = (int64_t)kmsg.msg_control;
        if (ctrl_ptr >= 0xFFFFFFFF80000000ULL ||
            ctrl_ptr + kmsg.msg_controllen > 0xFFFFFFFF80000000ULL) {
            kfree(kiov);
            file_put(sf);
            return (int64_t)-EFAULT;
        }
        if (kmsg.msg_controllen > 4096) {
            kfree(kiov);
            file_put(sf);
            return (int64_t)-EINVAL;
        }
        kcontrol = kmalloc(kmsg.msg_controllen);
        if (!kcontrol) { kfree(kiov); file_put(sf); return (int64_t)-ENOMEM; }
        copy_from_user(kcontrol, (const void __user *)kmsg.msg_control, kmsg.msg_controllen);
        kcontrollen = kmsg.msg_controllen;
    }

    struct unix_sock *sock = sf->sock;
    if (!sock) { kfree(kiov); if (kcontrol) kfree(kcontrol); file_put(sf); return (int64_t)-EBADF; }

    int64_t ret = sock_sendmsg_internal(sock, kiov, kmsg.msg_iovlen,
                                         kcontrol, kcontrollen, flags);

    kfree(kiov);
    if (kcontrol) kfree(kcontrol);
    file_put(sf);
    return (int64_t)ret;
}

int64_t sys_recvmsg(int64_t arg1, int64_t arg2, int64_t arg3, int64_t _u1, int64_t _u2, int64_t _u3) {
    int fd = (int)arg1;
    struct msghdr __user *msg = (struct msghdr __user *)arg2;
    int flags = (int)arg3;

    xtask_t *proc = current_task;

    if (fd < 0 || fd >= MAX_FD) return (int64_t)-EBADF;
    rcu_read_lock();
    struct file *rf = fd_lookup(proc->proc->files, fd);
    if (!rf || rf->type != FD_SOCKET) { rcu_read_unlock(); return (int64_t)-ENOTSOCK; }
    file_get(rf);
    rcu_read_unlock();

    uint64_t msg_ptr = (int64_t)msg;
    if (!msg_ptr || msg_ptr >= 0xFFFFFFFF80000000ULL ||
        msg_ptr + sizeof(struct msghdr) > 0xFFFFFFFF80000000ULL)
        { file_put(rf); return (int64_t)-EFAULT; }

    struct msghdr kmsg;
    copy_from_user(&kmsg, msg, sizeof(struct msghdr));

    // Validate iov
    uint64_t iov_ptr = (int64_t)kmsg.msg_iov;
    if (!iov_ptr || iov_ptr >= 0xFFFFFFFF80000000ULL ||
        iov_ptr + kmsg.msg_iovlen * sizeof(struct iovec) > 0xFFFFFFFF80000000ULL ||
        kmsg.msg_iovlen == 0 || kmsg.msg_iovlen > 1024)
        { file_put(rf); return (int64_t)-EINVAL; }

    struct iovec *kiov = (struct iovec *)kmalloc(kmsg.msg_iovlen * sizeof(struct iovec));
    if (!kiov) { file_put(rf); return (int64_t)-ENOMEM; }
    copy_from_user(kiov, (const void __user *)kmsg.msg_iov, kmsg.msg_iovlen * sizeof(struct iovec));

    for (size_t i = 0; i < kmsg.msg_iovlen; i++) {
        if (kiov[i].iov_base) {
            uint64_t base = (int64_t)kiov[i].iov_base;
            if (base >= 0xFFFFFFFF80000000ULL || base + kiov[i].iov_len > 0xFFFFFFFF80000000ULL) {
                kfree(kiov);
                file_put(rf);
                return (int64_t)-EFAULT;
            }
        }
    }

    // Control buffer for SCM_RIGHTS output
    void *kcontrol = NULL;
    size_t kcontrollen = 0;
    if (kmsg.msg_control && kmsg.msg_controllen > 0) {
        uint64_t ctrl_ptr = (int64_t)kmsg.msg_control;
        if (ctrl_ptr >= 0xFFFFFFFF80000000ULL ||
            ctrl_ptr + kmsg.msg_controllen > 0xFFFFFFFF80000000ULL) {
            kfree(kiov);
            file_put(rf);
            return (int64_t)-EFAULT;
        }
        kcontrol = (void *)ctrl_ptr;  // write directly to user space
        kcontrollen = kmsg.msg_controllen;
    }

    struct unix_sock *sock = rf->sock;
    if (!sock) { kfree(kiov); file_put(rf); return (int64_t)-EBADF; }

    int64_t ret = sock_recvmsg_internal(sock, kiov, kmsg.msg_iovlen,
                                         kcontrol, &kcontrollen, flags);

    // Update msg_controllen in user space
    if (ret >= 0) {
        if (kmsg.msg_control && kmsg.msg_controllen > 0) {
            // struct msghdr: offset 40 = msg_controllen (size_t)
            copy_to_user((void __user *)((char __user *)msg + 40), &kcontrollen, sizeof(size_t));
        }
    }

    kfree(kiov);
    file_put(rf);
    return (int64_t)ret;
}

int64_t sys_shutdown(int64_t arg1, int64_t arg2, int64_t _u1, int64_t _u2, int64_t _u3, int64_t _u4) {
    int fd = (int)arg1;
    int how = (int)arg2;

    xtask_t *proc = current_task;

    if (fd < 0 || fd >= MAX_FD) return (int64_t)-EBADF;
    rcu_read_lock();
    struct file *shf = fd_lookup(proc->proc->files, fd);
    if (!shf || shf->type != FD_SOCKET) { rcu_read_unlock(); return (int64_t)-ENOTSOCK; }
    file_get(shf);
    rcu_read_unlock();

    struct unix_sock *sock = shf->sock;
    if (!sock) { file_put(shf); return (int64_t)-EBADF; }

    if (how < 0 || how > 2) { file_put(shf); return (int64_t)-EINVAL; }

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
    // If we shut down write, peer's recvmsg should return EOF — wake peer's blocked reader
    // If we shut down read, peer's sendmsg should return EPIPE — wake peer's blocked writer
    pid_t peer_reader = -1, peer_writer = -1;
    if (sock->peer_sock) {
        if (how == SHUT_WR || how == SHUT_RDWR) {
            peer_reader = sock->peer_sock->blocked_reader;
            sock->peer_sock->blocked_reader = -1;
        }
        if (how == SHUT_RD || how == SHUT_RDWR) {
            peer_writer = sock->peer_sock->blocked_writer;
            sock->peer_sock->blocked_writer = -1;
        }
    }
    sock->blocked_reader = -1;
    sock->blocked_writer = -1;

    spin_unlock(&socket_lock);

    if (reader >= 0) wake_process(reader);
    if (writer >= 0) wake_process(writer);
    if (peer_reader >= 0) wake_process(peer_reader);
    if (peer_writer >= 0) wake_process(peer_writer);

    file_put(shf);
    return 0;
}

int64_t sys_poll(int64_t arg1, int64_t arg2, int64_t arg3, int64_t _u1, int64_t _u2, int64_t _u3) {
    struct pollfd __user *fds = (struct pollfd __user *)arg1;
    nfds_t nfds = (nfds_t)arg2;
    int timeout_ms = (int)arg3;

    xtask_t *proc = current_task;

    // Validate user pointer
    uint64_t fds_ptr = (int64_t)fds;
    if (!fds_ptr || fds_ptr >= 0xFFFFFFFF80000000ULL ||
        fds_ptr + nfds * sizeof(struct pollfd) > 0xFFFFFFFF80000000ULL)
        return (int64_t)-EFAULT;

    // Copy pollfd array to kernel
    struct pollfd *kfds = (struct pollfd *)kmalloc(nfds * sizeof(struct pollfd));
    if (!kfds) return (int64_t)-ENOMEM;
    copy_from_user(kfds, fds, nfds * sizeof(struct pollfd));

    uint64_t deadline = 0;
    if (timeout_ms > 0) {
        deadline = sched_clock() + (int64_t)timeout_ms * 1000000ULL;
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

            struct file *f;
            rcu_read_lock();
            f = fd_lookup(proc->proc->files, pfd);
            if (f) file_get(f);
            rcu_read_unlock();
            if (!f) {
                kfds[i].revents = POLLERR;
                ready++;
                continue;
            }

            if (f->type == FD_PIPE) {
                struct pipe *p = f->pipe;
                if (p) {
                    // POLLIN: data available or EOF (no writers)
                    if (p->head != p->tail || refcount_read(&p->p_count) <= 1) {
                        if (kfds[i].events & POLLIN) kfds[i].revents |= POLLIN;
                    }
                    // POLLOUT: space available or peer closed
                    if ((p->head + 1) % PIPE_BUF_SIZE != p->tail || refcount_read(&p->p_count) <= 1) {
                        if (kfds[i].events & POLLOUT) kfds[i].revents |= POLLOUT;
                    }
                }
            } else if (f->type == FD_SOCKET) {
                struct unix_sock *sock = f->sock;
                if (sock) {
                    spin_lock(&socket_lock);

                    // POLLIN: data available or shutdown_read
                    if (sock->recv_queue_head || sock->shutdown_read) {
                        if (kfds[i].events & POLLIN) kfds[i].revents |= POLLIN;
                    }

                    // POLLOUT: not shutdown_write
                    if (!sock->shutdown_write && sock->state == UNIX_CONNECTED) {
                        if (kfds[i].events & POLLOUT) kfds[i].revents |= POLLOUT;
                    }

                    // POLLHUP: peer gone (always reported regardless of events mask)
                    if (sock->state == UNIX_CLOSED ||
                        (sock->peer >= 0 && sock->peer < MAX_PROC &&
                         tasks[sock->peer].pid != sock->peer)) {
                        kfds[i].revents |= POLLHUP;
                    }

                    // For LISTEN socket: POLLIN = has pending connections
                    if (sock->state == UNIX_LISTEN && sock->backlog_len > 0) {
                        if (kfds[i].events & POLLIN) kfds[i].revents |= POLLIN;
                    }

                    spin_unlock(&socket_lock);
                }
            } else if (f->type == FD_FILE) {
                // FD_FILE: always writable (buffered), POLLIN if not at EOF
                if (kfds[i].events & POLLOUT) kfds[i].revents |= POLLOUT;
                // For POLLIN: check if offset < file_size
                if ((kfds[i].events & POLLIN) &&
                    f->file_data._offset < f->file_data.file_size) {
                    kfds[i].revents |= POLLIN;
                }
            } else if (f->type == FD_TTY) {
                struct pty *pty = f->pty;
                if (pty) {
                    int is_master = pty_fd_is_master(proc->proc->files, i);
                    __poll_t revents = pty_poll(pty, is_master, kfds[i].events);
                    kfds[i].revents |= revents;
                }
            } else if (f->type == FD_DEV) {
                // FD_DEV: call dev_ops.poll callback for kernel devices
                struct inode *ip = f->inode;
                if (ip && ip->i_priv) {
                    struct dev_ops *ops = (struct dev_ops *)ip->i_priv;
                    if (ops->driver_pid == 0 && ops->poll) {
                        __poll_t revents = ops->poll(current_task, kfds[i].events);
                        kfds[i].revents |= revents;
                    }
                }
                // User-space driver: always ready (IPC handles events)
                if (f->target_pid > 0) {
                    if (kfds[i].events & POLLIN) kfds[i].revents |= POLLIN;
                    if (kfds[i].events & POLLOUT) kfds[i].revents |= POLLOUT;
                }
            } else {
                // FD_SHM etc: always ready
                if (kfds[i].events & POLLIN) kfds[i].revents |= POLLIN;
                if (kfds[i].events & POLLOUT) kfds[i].revents |= POLLOUT;
            }
            if (kfds[i].revents) ready++;
            file_put(f);
        }

        if (ready > 0) {
            // Copy results back to user
            copy_to_user(fds, kfds, nfds * sizeof(struct pollfd));
            kfree(kfds);
            return (int64_t)ready;
        }

        if (timeout_ms == 0) {
            copy_to_user(fds, kfds, nfds * sizeof(struct pollfd));
            kfree(kfds);
            return 0;
        }

        // Block on WAIT_POLL
        if (deadline > 0) {
            uint64_t now = sched_clock();
            if (now >= deadline) {
                __memset(kfds, 0, nfds * sizeof(struct pollfd));
                copy_to_user(fds, kfds, nfds * sizeof(struct pollfd));
                kfree(kfds);
                return 0;  // timeout
            }
            proc->wait_deadline = deadline;
            uint64_t pflags;
            spin_lock_irqsave(&cpu_locals[proc->assigned_cpu].scheduler_lock, &pflags);
            timer_queue_insert(proc->assigned_cpu, proc);
            spin_unlock_irqrestore(&cpu_locals[proc->assigned_cpu].scheduler_lock, pflags);
        } else {
            proc->wait_deadline = 0;
        }

        proc->state = BLOCKED;
        proc->wait_event = WAIT_POLL;
        proc->wait_timed_out = 0;
        schedule();

        // EINTR check (before timeout check: signal priority over timeout)
        {
            uint64_t pend = __atomic_load_n(&proc->proc->sig_pending, __ATOMIC_ACQUIRE);
            uint64_t deliv = pend & ~proc->proc->sig_blocked;
            deliv |= (pend & ((1ULL << SIGKILL) | (1ULL << SIGSTOP)));
            if (deliv) {
                kfree(kfds);
                return (int64_t)-EINTR;
            }
        }

        if (proc->wait_timed_out && timeout_ms > 0) {
            __memset(kfds, 0, nfds * sizeof(struct pollfd));
            copy_to_user(fds, kfds, nfds * sizeof(struct pollfd));
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

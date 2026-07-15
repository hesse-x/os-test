/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arch/x64/apic.h"
#include "arch/x64/smp.h"
#include "arch/x64/utils.h"
#include "kernel/bsd/file_poll.h"
#include "kernel/bsd/inode.h"
#include "kernel/bsd/mount.h"
#include "kernel/bsd/netlink.h"
#include "kernel/bsd/proc.h"
#include "kernel/bsd/socket.h"
#include "kernel/bsd/types.h"
#include "kernel/bsd/vfs.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/kasan.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/rcu.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/xtask.h"
#include <xos/errno.h>
#include <xos/fcntl.h>
#include <xos/netlink.h>
#include <xos/signal.h>
#include <xos/socket.h>
#include <xos/stat.h>

// ===================== Global socket lock =====================
spinlock socket_lock = SPINLOCK_INIT;

// ===================== Bind name space =====================
static struct unix_bind_entry *unix_bind_table[UNIX_HASH_SIZE];

static uint32_t unix_hash(const char *path) {
  uint32_t h = 0;
  for (int i = 0; path[i] && i < 108; i++) {
    h = h * 31 + (uint8_t)path[i];
  }
  return h % UNIX_HASH_SIZE;
}

static int unix_bind_lookup_hash(const char *sun_path, struct unix_sock **out,
                                 pid_t *owner_pid) {
  uint32_t h = unix_hash(sun_path);
  for (struct unix_bind_entry *e = unix_bind_table[h]; e; e = e->next) {
    int i = 0;
    while (i < 108 && sun_path[i] == e->sun_path[i]) {
      if (sun_path[i] == '\0')
        break;
      i++;
    }
    if (i < 108 && sun_path[i] == '\0' && e->sun_path[i] == '\0') {
      if (e->sock && e->sock->state == UNIX_LISTEN) {
        *out = e->sock;
        *owner_pid = e->owner_pid;
        return 0;
      }
      return -ENOENT; // exists but not listening
    }
  }
  return -ENOENT;
}

static int unix_bind_register_hash(const char *sun_path, struct unix_sock *sock,
                                   pid_t owner_pid) {
  // Check for duplicate (already bound to some socket — not necessarily LISTEN)
  uint32_t h = unix_hash(sun_path);
  for (struct unix_bind_entry *e = unix_bind_table[h]; e; e = e->next) {
    int i = 0;
    while (i < 108 && sun_path[i] == e->sun_path[i]) {
      if (sun_path[i] == '\0')
        break;
      i++;
    }
    if (i < 108 && sun_path[i] == '\0' && e->sun_path[i] == '\0') {
      return -EADDRINUSE;
    }
  }

  struct unix_bind_entry *entry =
      (struct unix_bind_entry *)kmalloc(sizeof(struct unix_bind_entry));
  if (!entry)
    return -ENOMEM;

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

/* ===== VFS socket inode helpers（/run tmpfs 上 mknod socket）=====
 * 调用者持 socket_lock；本层取 mount_lock(vfs_resolve) + inode_hash_lock +
 * tmpfs_i_lock，锁序单向 socket_lock→mount_lock→inode_hash_lock→tmpfs_i_lock，
 * 无反向获取路径（inode_hash_lock 仅 inode.c 用，mount_lock 仅 mount.c 用）。
 */

/* VFS 路径：path_walk 父目录 + i_op->create(S_IFSOCK) 建 socket inode。
 * 返 ERR_PTR(-errno) 失败；成功返 +1 引用 inode（i_priv 初始 NULL，待挂
 * sock）。 */
static struct inode *vfs_mknod_socket(const char *sun_path) {
  char relpath[256], lastname[256];
  struct mount_entry *m = vfs_resolve(sun_path, relpath, sizeof(relpath));
  if (!m)
    return ERR_PTR(-ENOENT);
  struct inode *parent = NULL;
  int rc = path_walk_parent(m, relpath, &parent, lastname, sizeof(lastname));
  if (rc) {
    if (parent)
      inode_put(parent);
    return ERR_PTR((long)rc);
  }
  if (!parent->i_op || !parent->i_op->create) {
    inode_put(parent);
    return ERR_PTR(-EOPNOTSUPP);
  }
  struct inode *ip =
      parent->i_op->create(parent, lastname, (int)(S_IFSOCK | 0777));
  inode_put(parent);
  if (IS_ERR(ip))
    return ip;
  return ip; /* +1 引用，挂 sock 用 */
}

/* VFS 路径：path_walk 取 socket inode（+1 引用）。 */
static struct inode *vfs_lookup_socket(const char *sun_path) {
  char relpath[256];
  struct mount_entry *m = vfs_resolve(sun_path, relpath, sizeof(relpath));
  if (!m)
    return ERR_PTR(-ENOENT);
  struct inode *ip = path_walk(m, relpath); /* +1 */
  if (!ip)
    return ERR_PTR(-ENOENT);
  return ip;
}

/* 新 register：先走 VFS（建 socket inode + 挂 sock），失败降级哈希表占名。
 * 对齐 Linux bind 语义：路径已存在（无论是否已 bind）→ EADDRINUSE。 */
int unix_bind_register(const char *sun_path, struct unix_sock *sock,
                       pid_t owner_pid) {
  struct inode *ip = vfs_mknod_socket(sun_path);
  if (!IS_ERR(ip) && ip) {
    /* create 成功（文件此前不存在）→ 挂 sock */
    ip->i_priv = sock;     /* sock 挂 inode->i_priv */
    sock->bind_inode = ip; /* 记录，unregister 时清 i_priv + inode_put */
    sock->owner_pid = owner_pid;
    /* 不 inode_put：bind 期间持有 inode 引用 */
    return 0;
  }
  /* create 返 EEXIST：路径已存在 → 对齐 Linux 返 EADDRINUSE（非 EEXIST） */
  if (IS_ERR(ip) && PTR_ERR(ip) == -EEXIST) {
    return -EADDRINUSE;
  }
  /* 其余 VFS 失败（/run 未挂载 / path 解析失败）→ 降级哈希表占名 */
  return unix_bind_register_hash(sun_path, sock, owner_pid);
}

/* 新 lookup：先走 VFS，失败降级哈希表。
 * 对齐 Linux connect 语义：路径存在但未 LISTEN → -ECONNREFUSED（非 -ENOENT）。
 */
int unix_bind_lookup(const char *sun_path, struct unix_sock **out,
                     pid_t *owner_pid) {
  struct inode *ip = vfs_lookup_socket(sun_path);
  if (!IS_ERR(ip) && ip) {
    struct unix_sock *s = (struct unix_sock *)ip->i_priv;
    if (s && s->state == UNIX_LISTEN) {
      *out = s;
      *owner_pid = s->owner_pid;
      inode_put(ip);
      return 0;
    }
    inode_put(ip);
    /* socket 文件存在但未 LISTEN（或 i_priv 空）→ ECONNREFUSED */
    return -ECONNREFUSED;
  }
  return unix_bind_lookup_hash(sun_path, out, owner_pid);
}

void unix_bind_unregister(struct unix_sock *sock) {
  /* VFS 路径清理：清 inode->i_priv + 释放 bind 期间持有的 inode 引用。
   * bind_inode 为 NULL 时 no-op（哈希表路径）。 */
  if (sock->bind_inode) {
    sock->bind_inode->i_priv = NULL;
    inode_put(sock->bind_inode);
    sock->bind_inode = NULL;
  }
  if (!sock->sun_path[0])
    return; // not bound (哈希表路径未建 inode)
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
  struct sk_buff *skb =
      (struct sk_buff *)kmalloc(sizeof(struct sk_buff) + data_len);
  if (!skb)
    return NULL;
  skb->next = NULL;
  skb->len = data_len;
  skb->consumed = 0;
  skb->num_fds = 0;
  return skb;
}

void skb_free(struct sk_buff *skb) {
  if (skb)
    kfree(skb);
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
  if (!sock->recv_queue_head)
    return NULL;
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
  struct unix_sock *sock =
      (struct unix_sock *)kmalloc(sizeof(struct unix_sock));
  if (!sock)
    return NULL;
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
  sock->bind_inode = NULL;
  sock->owner_pid = -1;
  sock->wq = NULL;
  return sock;
}

void unix_sock_free(struct unix_sock *sock) {
  if (!sock)
    return;
  // Free any remaining skbs in recv queue
  while (sock->recv_queue_head) {
    struct sk_buff *skb = skb_dequeue(sock);
    skb_free(skb);
  }
  // Free backlog connections
  struct unix_sock *bp = sock->backlog_head;
  while (bp) {
    struct unix_sock *next = bp->backlog_head; // backlog_head is next in chain
    // Tell peer we're closing
    if (bp->peer >= 0) {
      xtask *peer = task_get(bp->peer);
      if (peer->pid == bp->peer) {
        // Wake peer if waiting on this socket
        unix_sock_wake_reader(bp);
        unix_sock_wake_writer(bp);
      }
    }
    // Detach peer's back-reference so it cannot dereference us after free.
    if (bp->peer_sock) {
      spin_lock(&socket_lock);
      bp->peer_sock->peer_sock = NULL;
      spin_unlock(&socket_lock);
    }
    // Free the child socket
    bp->peer = -1;
    bp->state = UNIX_CLOSED;
    // Free any skbs
    while (bp->recv_queue_head) {
      struct sk_buff *skb2 = skb_dequeue(bp);
      skb_free(skb2);
    }
    if (bp->wq)
      kfree(bp->wq);
    kfree(bp);
    bp = next;
  }
  if (sock->wq)
    kfree(sock->wq);
  kfree(sock);
}

void unix_sock_release(struct unix_sock *sock) {
  if (!sock)
    return;
  if (refcount_dec_and_test(&sock->u_count)) {
    unix_bind_unregister(sock);
    unix_sock_free(sock);
  }
}

// ===================== Wake helpers =====================

void unix_sock_wake_reader(struct unix_sock *sock) {
  if (sock->blocked_reader >= 0) {
    wake_process(sock->blocked_reader);
  }
  if (sock->wq)
    __wake_up(sock->wq, POLLIN);
}

void unix_sock_wake_writer(struct unix_sock *sock) {
  if (sock->blocked_writer >= 0) {
    wake_process(sock->blocked_writer);
  }
  if (sock->wq)
    __wake_up(sock->wq, POLLOUT);
}

// ===================== Internal sendmsg/recvmsg =====================

int64_t unix_sock_sendmsg(struct unix_sock *sock, const struct iovec *iov,
                          size_t iovlen, const void *control, size_t controllen,
                          int flags) {
  // Check shutdown_write on our socket
  if (sock->shutdown_write)
    return -EPIPE;
  // Check peer shutdown_read — sending to a socket whose read side is shut down
  // should EPIPE
  if (sock->peer_sock && sock->peer_sock->shutdown_read)
    return -EPIPE;

  // Calculate total data length
  uint32_t total = 0;
  for (size_t i = 0; i < iovlen; i++) {
    total += iov[i].iov_len;
  }
  if (total > MAX_SOCKET_DATA)
    return -EMSGSIZE;

  // Allocate skb
  struct sk_buff *skb = skb_alloc(total);
  if (!skb)
    return -ENOMEM;

  // Copy data from iovecs
  uint32_t offset = 0;
  for (size_t i = 0; i < iovlen; i++) {
    if (iov[i].iov_base && iov[i].iov_len > 0) {
      if (copy_from_user(skb->data + offset,
                         (const void __user *)iov[i].iov_base,
                         iov[i].iov_len)) {
        skb_free(skb);
        return -EFAULT;
      }
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
      if (aligned_len > (uint32_t)(cmsg_end - cmsg_ptr))
        break;
      if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
        int num_fds =
            (cmsg->cmsg_len - CMSG_ALIGN(sizeof(struct cmsghdr))) / sizeof(int);
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

  // Find peer socket via direct pointer (socketpair) or PID-based lookup
  // (connect)
  struct unix_sock *peer_sock = sock->peer_sock;
  if (!peer_sock) {
    // peer_sock is always set during connect/socketpair; this should not be
    // reached
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

  // If O_NONBLOCK and queue is "full" (more than a reasonable limit), return
  // EAGAIN
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
  if (peer_sock->wq)
    __wake_up(peer_sock->wq, POLLIN);

  return (int64_t)total;
}

int64_t unix_sock_recvmsg(struct unix_sock *sock, const struct iovec *iov,
                          size_t iovlen, void *control, size_t *controllen,
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
        return 0; // EOF
      }
      // 2) Peer shutdown_write (we will never receive more data)
      if (sock->peer_sock && sock->peer_sock->shutdown_write) {
        spin_unlock(&socket_lock);
        return 0; // EOF — peer shut down write side
      }
      // 3) Socket closed or peer zombie
      if (sock->state == UNIX_CLOSED ||
          (sock->peer >= 0 && task_get(sock->peer)->pid == sock->peer &&
           task_get(sock->peer)->state == ZOMBIE)) {
        // Check if peer has closed. peer_sock is NULL after peer close
        // detached the back-reference (see unix_sock_close).
        bool peer_closed = true;
        if (sock->peer_sock) {
          peer_closed = (sock->peer_sock->state != UNIX_CONNECTED);
        }
        if (peer_closed) {
          spin_unlock(&socket_lock);
          return 0; // EOF
        }
      }

      if (nonblock) {
        spin_unlock(&socket_lock);
        return -EAGAIN;
      }

      // Block reader
      sock->blocked_reader = current_task->pid;
      xtask *proc = current_task;
      proc->state = BLOCKED;
      proc->wait_event = WAIT_POLL;
      proc->wait_deadline =
          sched_clock() + 30000000000ULL; // 30s default timeout
      spin_unlock(&socket_lock);
      int cpu = proc->assigned_cpu;
      uint64_t rflags;
      spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &rflags);
      sched_timer_queue_insert(cpu, proc);
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
        uint64_t pend =
            __atomic_load_n(&proc->proc->sig_pending, __ATOMIC_ACQUIRE);
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
      if (copy > avail - to_read)
        copy = avail - to_read;
      to_read += copy;
    }

    // Copy data to iovecs
    uint32_t data_offset = skb->consumed;
    uint32_t remaining = to_read;
    for (size_t i = 0; i < iovlen && remaining > 0; i++) {
      if (iov[i].iov_base && iov[i].iov_len > 0) {
        uint32_t copy = (uint32_t)iov[i].iov_len;
        if (copy > remaining)
          copy = remaining;
        if (copy_to_user((void __user *)iov[i].iov_base,
                         skb->data + data_offset, copy)) {
          spin_unlock(&socket_lock);
          return -EFAULT;
        }
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
      for (int i = 0; i < skb->num_fds && num_fds_to_install < SCM_MAX_FD;
           i++) {
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
      xtask *proc = current_task;
      for (int i = 0; i < num_fds_to_install && num_fds_installed < SCM_MAX_FD;
           i++) {
        int orig_fd = orig_fds[i];
        // Step 1: RCU read — validate + file_get (atomic verify+hold, no
        // TOCTOU)
        struct file *src = NULL;
        if (sender_pid >= 0 && sender_pid < MAX_PROC) {
          xtask *sender = task_get(sender_pid);
          if (sender->pid == sender_pid && sender->mm && sender->proc->files &&
              orig_fd >= 0 && orig_fd < MAX_FD) {
            rcu_read_lock();
            src = fd_lookup(sender->proc->files, orig_fd);
            if (src)
              file_get(src); // hold ref, sender close won't free
            rcu_read_unlock();
          }
        }
        if (!src)
          continue;

        // Step 2: receiver_fd_lock — find free slot + install pointer
        spinlock *fdlk = &proc->proc->files->fd_lock;
        spin_lock(fdlk);
        int new_fd = alloc_fd(proc->proc->files, 0);
        if (new_fd < 0) {
          spin_unlock(fdlk);
          file_put(src);
          break; // no free fd slots
        }
        fd_install(proc->proc->files, new_fd,
                   src); // install pointer directly, no extra bump needed
        if (src->type == FD_TTY)
          pty_dup_file(src);
        spin_unlock(fdlk);

        installed_fds[num_fds_installed++] = new_fd;
      }
    }

    // Write SCM_RIGHTS to control buffer
    if (num_fds_installed > 0 && control && controllen && *controllen > 0) {
      struct cmsghdr *cmsg = (struct cmsghdr *)control;
      size_t needed =
          CMSG_ALIGN(sizeof(struct cmsghdr)) + num_fds_installed * sizeof(int);
      if (*controllen >= needed) {
        cmsg->cmsg_len = (uint32_t)(CMSG_ALIGN(sizeof(struct cmsghdr)) +
                                    num_fds_installed * sizeof(int));
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

void unix_sock_close(struct unix_sock *sock) {
  if (!sock)
    return;

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
  pid_t peer_reader = -1, peer_writer = -1;
  if (peer_s) {
    peer_reader = peer_s->blocked_reader;
    peer_writer = peer_s->blocked_writer;
    peer_s->blocked_reader = -1;
    peer_s->blocked_writer = -1;
    // Mark the peer's read side as shutdown so subsequent recv/poll on the
    // peer sees EOF (POLLIN with read returning 0), matching Linux semantics
    // for close() of the remote end.
    peer_s->shutdown_read = 1;
    // Detach back-reference so the peer never dereferences us after free.
    peer_s->peer_sock = NULL;
  }
  sock->blocked_reader = -1;
  sock->blocked_writer = -1;

  spin_unlock(&socket_lock);

  // Wake our blocked reader/writer outside lock
  if (reader >= 0)
    wake_process(reader);
  if (writer >= 0)
    wake_process(writer);

  // Wake peer's blocked reader/writer so it can detect EOF/EPIPE
  if (peer_reader >= 0)
    wake_process(peer_reader);
  if (peer_writer >= 0)
    wake_process(peer_writer);
  // Note: no PID-based fallback wake. When peer_s is NULL the peer socket was
  // already closed; its process may be in any wait state (e.g. WAIT_CHILD)
  // and wake_process() ASSERTs on non-IPC events. Socket-level wakeups are
  // already covered by peer_reader/peer_writer and the __wake_up() calls below.

  // Notify epoll-style waiters of close-driven readiness (POLLHUP).
  if (sock->wq)
    __wake_up(sock->wq, POLLHUP | POLLIN | POLLOUT);
  if (peer_s && peer_s->wq)
    __wake_up(peer_s->wq, POLLHUP | POLLIN | POLLOUT);

  // Release reference (actual free when ref_count hits 0)
  unix_sock_release(sock);
}

// ===================== Syscall implementations =====================

int64_t sys_socket(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                   int64_t unused2, int64_t unused3) {
  int domain = (int)arg1;
  int type = (int)arg2;
  int protocol = (int)arg3;

  // Strip socket type flags before checking base type
  int sock_flags = type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK);
  int base_type = type & (SOCK_STREAM | SOCK_DGRAM | SOCK_SEQPACKET);

  // Only AF_UNIX SOCK_STREAM supported
  if (domain == AF_NETLINK) {
    if (base_type != SOCK_DGRAM)
      return (int64_t)-ESOCKTNOSUPPORT;
    struct netlink_sock *nl = netlink_sock_alloc(protocol);
    if (!nl)
      return (int64_t)-ENOMEM;

    xtask *proc = current_task;
    spinlock *fdlk = &proc->proc->files->fd_lock;
    spin_lock(fdlk);

    int fd = alloc_fd(proc->proc->files, 3);
    if (fd < 0) {
      spin_unlock(fdlk);
      netlink_sock_release(nl);
      return (int64_t)-EMFILE;
    }

    struct file *f = (struct file *)kmalloc(sizeof(struct file));
    if (!f) {
      spin_unlock(fdlk);
      netlink_sock_release(nl);
      return (int64_t)-ENOMEM;
    }
    __memset(f, 0, sizeof(*f));
    refcount_set(&f->f_count, 1);
    f->type = FD_NETLINK;
    f->flags = O_RDWR;
    if (sock_flags & SOCK_NONBLOCK)
      f->flags |= O_NONBLOCK;
    if (sock_flags & SOCK_CLOEXEC)
      f->flags |= FD_CLOEXEC;
    f->nlsock = nl;
    fd_install(proc->proc->files, fd, f);

    spin_unlock(fdlk);
    return (int64_t)fd;
  }

  if (domain != AF_UNIX)
    return (int64_t)-EAFNOSUPPORT;
  if (base_type != SOCK_STREAM)
    return (int64_t)-EPROTONOSUPPORT;
  if (protocol != 0)
    return (int64_t)-EPROTONOSUPPORT;

  struct unix_sock *sock = unix_sock_alloc();
  if (!sock)
    return (int64_t)-ENOMEM;

  xtask *proc = current_task;

  sock->state = UNIX_FREE;

  spinlock *fdlk = &proc->proc->files->fd_lock;
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
  if (sock_flags & SOCK_NONBLOCK)
    f->flags |= O_NONBLOCK;
  if (sock_flags & SOCK_CLOEXEC)
    f->flags |= FD_CLOEXEC;
  f->sock = sock;
  fd_install(proc->proc->files, fd, f);

  spin_unlock(fdlk);

  return (int64_t)fd;
}

int64_t sys_bind(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                 int64_t unused2, int64_t unused3) {
  int fd = (int)arg1;
  const struct sockaddr_un __user *addr =
      (const struct sockaddr_un __user *)arg2;
  socklen_t addrlen = (socklen_t)arg3;

  xtask *proc = current_task;

  // Validate fd
  if (fd < 0 || fd >= MAX_FD)
    return (int64_t)-EBADF;
  rcu_read_lock();
  struct file *bf = fd_lookup(proc->proc->files, fd);
  if (!bf || (bf->type != FD_SOCKET && bf->type != FD_NETLINK)) {
    rcu_read_unlock();
    return (int64_t)-ENOTSOCK;
  }
  file_get(bf);
  rcu_read_unlock();

  // Validate addr
  uint64_t addr_ptr = (int64_t)addr;
  if (!addr_ptr || addr_ptr >= 0xFFFFFFFF80000000ULL ||
      addr_ptr + addrlen > 0xFFFFFFFF80000000ULL || addrlen <= 0) {
    file_put(bf);
    return (int64_t)-EFAULT;
  }

  // Read sun_family
  uint16_t sun_family;
  if (copy_from_user(&sun_family, addr, sizeof(sun_family))) {
    file_put(bf);
    return (int64_t)-EFAULT;
  }
  if (sun_family == AF_NETLINK) {
    // Validate addr pointer for sockaddr_nl
    if (addrlen < sizeof(sockaddr_nl)) {
      file_put(bf);
      return (int64_t)-EINVAL;
    }
    if (bf->type != FD_NETLINK) {
      file_put(bf);
      return (int64_t)-ENOTSOCK;
    }
    struct netlink_sock *nlsock = bf->nlsock;
    if (!nlsock) {
      file_put(bf);
      return (int64_t)-EBADF;
    }
    sockaddr_nl nl_addr;
    if (copy_from_user(&nl_addr, addr, sizeof(sockaddr_nl))) {
      file_put(bf);
      return (int64_t)-EFAULT;
    }
    int64_t ret = netlink_sock_bind(nlsock, &nl_addr);
    file_put(bf);
    return ret;
  }

  if (sun_family != AF_UNIX) {
    file_put(bf);
    return (int64_t)-EAFNOSUPPORT;
  }

  // AF_UNIX bind must use FD_SOCKET
  if (bf->type != FD_SOCKET) {
    file_put(bf);
    return (int64_t)-ENOTSOCK;
  }

  // Read sun_path (max 108 bytes)
  char sun_path[108];
  __memset(sun_path, 0, sizeof(sun_path));
  size_t path_len = addrlen - sizeof(sun_family);
  if (path_len > 107)
    path_len = 107;
  if (path_len > 0) {
    if (copy_from_user(sun_path, (const char __user *)addr + sizeof(sun_family),
                       path_len)) {
      file_put(bf);
      return (int64_t)-EFAULT;
    }
  }
  sun_path[107] = '\0';

  if (sun_path[0] == '\0') {
    file_put(bf);
    return (int64_t)-EINVAL;
  } // empty path

  struct unix_sock *sock = bf->sock;
  if (!sock) {
    file_put(bf);
    return (int64_t)-EBADF;
  }

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
    return (int64_t)ret;
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

int64_t sys_listen(int64_t arg1, int64_t arg2, int64_t unused1, int64_t unused2,
                   int64_t unused3, int64_t unused4) {
  int fd = (int)arg1;
  int backlog = (int)arg2;

  xtask *proc = current_task;

  if (fd < 0 || fd >= MAX_FD)
    return (int64_t)-EBADF;
  rcu_read_lock();
  struct file *lf = fd_lookup(proc->proc->files, fd);
  if (!lf || lf->type != FD_SOCKET) {
    rcu_read_unlock();
    return (int64_t)-ENOTSOCK;
  }
  file_get(lf);
  rcu_read_unlock();

  struct unix_sock *sock = lf->sock;
  if (!sock) {
    file_put(lf);
    return (int64_t)-EBADF;
  }

  if (backlog <= 0)
    backlog = 1;
  if (backlog > UNIX_MAX_BACKLOG)
    backlog = UNIX_MAX_BACKLOG;

  spin_lock(&socket_lock);
  sock->state = UNIX_LISTEN;
  sock->backlog_max = backlog;
  spin_unlock(&socket_lock);

  file_put(lf);
  return 0;
}

int64_t sys_accept(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                   int64_t unused2, int64_t unused3) {
  int fd = (int)arg1;
  struct sockaddr_un __user *addr = (struct sockaddr_un __user *)arg2;
  socklen_t __user *addrlen = (socklen_t __user *)arg3;

  xtask *proc = current_task;

  if (fd < 0 || fd >= MAX_FD)
    return (int64_t)-EBADF;
  rcu_read_lock();
  struct file *af = fd_lookup(proc->proc->files, fd);
  if (!af || af->type != FD_SOCKET) {
    rcu_read_unlock();
    return (int64_t)-ENOTSOCK;
  }
  file_get(af);
  rcu_read_unlock();

  struct unix_sock *listen_sock = af->sock;
  if (!listen_sock) {
    file_put(af);
    return (int64_t)-EBADF;
  }

  // Validate addr pointers if non-null
  if (addr) {
    uint64_t aptr = (int64_t)addr;
    if (aptr >= 0xFFFFFFFF80000000ULL) {
      file_put(af);
      return (int64_t)-EFAULT;
    }
  }
  if (addrlen) {
    uint64_t alptr = (int64_t)addrlen;
    if (alptr >= 0xFFFFFFFF80000000ULL ||
        alptr + sizeof(socklen_t) > 0xFFFFFFFF80000000ULL) {
      file_put(af);
      return (int64_t)-EFAULT;
    }
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
      // No pending connections: non-blocking fd → EAGAIN immediately
      if (af->flags & O_NONBLOCK) {
        spin_unlock(&socket_lock);
        ret = -EAGAIN;
        goto out;
      }
      // Blocking: wait with 30s timeout
      listen_sock->blocked_reader = proc->pid;
      proc->state = BLOCKED;
      proc->wait_event = WAIT_POLL;
      proc->wait_deadline = sched_clock() + 30000000000ULL; // 30 second timeout
      spin_unlock(&socket_lock);
      int cpu = proc->assigned_cpu;
      uint64_t rflags;
      spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &rflags);
      sched_timer_queue_insert(cpu, proc);
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
        uint64_t pend =
            __atomic_load_n(&proc->proc->sig_pending, __ATOMIC_ACQUIRE);
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
      continue; // re-check backlog after wake
    }

    // Dequeue from backlog
    listen_sock->backlog_head = child->backlog_head;
    if (!listen_sock->backlog_head) {
      listen_sock->backlog_tail = NULL;
    }
    listen_sock->backlog_len--;
    spin_unlock(&socket_lock);

    // Allocate new fd for this child socket (fd_lock only, no socket_lock
    // nesting)
    spinlock *fdlk = &proc->proc->files->fd_lock;
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
      // Find peer's sun_path from peer socket (direct pointer, set during
      // connect)
      struct unix_sock *peer_sock = child->peer_sock;
      if (peer_sock && peer_sock->sun_path[0]) {
        int pi = 0;
        while (peer_sock->sun_path[pi] && pi < 107) {
          sa.sun_path[pi] = peer_sock->sun_path[pi];
          pi++;
        }
        sa.sun_path[pi] = '\0';
      }
      // Copy to user. The accepted fd is already installed; on copy failure we
      // skip the address backfill rather than tear down a live connection.
      socklen_t user_alen = alen;
      if (!copy_from_user(&user_alen, addrlen, sizeof(socklen_t)) &&
          user_alen > alen)
        user_alen = alen;
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

int64_t sys_connect(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                    int64_t unused2, int64_t unused3) {
  int fd = (int)arg1;
  const struct sockaddr_un __user *addr =
      (const struct sockaddr_un __user *)arg2;
  socklen_t addrlen = (socklen_t)arg3;

  xtask *proc = current_task;

  if (fd < 0 || fd >= MAX_FD)
    return (int64_t)-EBADF;
  rcu_read_lock();
  struct file *cf = fd_lookup(proc->proc->files, fd);
  if (!cf || cf->type != FD_SOCKET) {
    rcu_read_unlock();
    return (int64_t)-ENOTSOCK;
  }
  file_get(cf);
  rcu_read_unlock();

  // Validate addr
  uint64_t addr_ptr = (int64_t)addr;
  if (!addr_ptr || addr_ptr >= 0xFFFFFFFF80000000ULL ||
      addr_ptr + addrlen > 0xFFFFFFFF80000000ULL || addrlen <= 0) {
    file_put(cf);
    return (int64_t)-EFAULT;
  }

  uint16_t sun_family;
  if (copy_from_user(&sun_family, addr, sizeof(sun_family))) {
    file_put(cf);
    return (int64_t)-EFAULT;
  }
  if (sun_family != AF_UNIX) {
    file_put(cf);
    return (int64_t)-EAFNOSUPPORT;
  }

  char sun_path[108];
  __memset(sun_path, 0, sizeof(sun_path));
  size_t path_len = addrlen - sizeof(sun_family);
  if (path_len > 107)
    path_len = 107;
  if (path_len > 0) {
    if (copy_from_user(sun_path, (const char __user *)addr + sizeof(sun_family),
                       path_len)) {
      file_put(cf);
      return (int64_t)-EFAULT;
    }
  }
  sun_path[107] = '\0';

  if (sun_path[0] == '\0') {
    file_put(cf);
    return (int64_t)-EINVAL;
  }

  spin_lock(&socket_lock);

  // Look up listener and owner PID via sun_path hash
  struct unix_sock *listener = NULL;
  pid_t listener_pid = -1;
  int ret = unix_bind_lookup(sun_path, &listener, &listener_pid);
  if (ret != 0) {
    spin_unlock(&socket_lock);
    file_put(cf);
    return (int64_t)ret;
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
  client_sock->peer_sock = child; // client's peer is the child (server-side)
  child->peer = proc->pid;
  child->peer_sock = client_sock; // child's peer is the client

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
  if (listener->wq)
    __wake_up(listener->wq, POLLIN);

  file_put(cf);
  return 0;
}

int64_t sys_socketpair(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4,
                       int64_t unused1, int64_t unused2) {
  int domain = (int)arg1;
  int type = (int)arg2;
  int protocol = (int)arg3;
  int __user *sv = (int __user *)arg4;

  if (domain != AF_UNIX)
    return (int64_t)-EAFNOSUPPORT;
  if (type != SOCK_STREAM)
    return (int64_t)-EPROTONOSUPPORT;
  if (protocol != 0)
    return (int64_t)-EPROTONOSUPPORT;

  // Validate user pointer
  uint64_t sv_ptr = (int64_t)sv;
  if (!sv_ptr || sv_ptr >= 0xFFFFFFFF80000000ULL ||
      sv_ptr + 2 * sizeof(int) > 0xFFFFFFFF80000000ULL)
    return (int64_t)-EFAULT;

  xtask *proc = current_task;

  struct unix_sock *a = unix_sock_alloc();
  struct unix_sock *b = unix_sock_alloc();
  if (!a || !b) {
    if (a)
      unix_sock_free(a);
    if (b)
      unix_sock_free(b);
    return (int64_t)-ENOMEM;
  }

  a->state = UNIX_CONNECTED;
  a->peer = proc->pid;
  a->peer_sock = b;
  b->state = UNIX_CONNECTED;
  b->peer = proc->pid;
  b->peer_sock = a;

  spinlock *fdlk = &proc->proc->files->fd_lock;
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
    if (fa)
      kfree(fa);
    if (fb)
      kfree(fb);
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
  int fds[2] = {fd_a, fd_b};
  if (copy_to_user(sv, fds, sizeof(fds)))
    return (int64_t)-EFAULT;

  return 0;
}

int64_t sys_sendmsg(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                    int64_t unused2, int64_t unused3) {
  int fd = (int)arg1;
  const struct msghdr __user *msg = (const struct msghdr __user *)arg2;
  int flags = (int)arg3;

  xtask *proc = current_task;

  if (fd < 0 || fd >= MAX_FD)
    return (int64_t)-EBADF;
  rcu_read_lock();
  struct file *sf = fd_lookup(proc->proc->files, fd);
  if (!sf || (sf->type != FD_SOCKET && sf->type != FD_NETLINK)) {
    rcu_read_unlock();
    return (int64_t)-ENOTSOCK;
  }
  file_get(sf);
  rcu_read_unlock();

  // Validate msghdr pointer
  uint64_t msg_ptr = (int64_t)msg;
  if (!msg_ptr || msg_ptr >= 0xFFFFFFFF80000000ULL ||
      msg_ptr + sizeof(struct msghdr) > 0xFFFFFFFF80000000ULL) {
    file_put(sf);
    return (int64_t)-EFAULT;
  }

  struct msghdr kmsg;
  if (copy_from_user(&kmsg, msg, sizeof(struct msghdr))) {
    file_put(sf);
    return (int64_t)-EFAULT;
  }

  // Validate iov
  uint64_t iov_ptr = (int64_t)kmsg.msg_iov;
  if (!iov_ptr || iov_ptr >= 0xFFFFFFFF80000000ULL ||
      iov_ptr + kmsg.msg_iovlen * sizeof(struct iovec) >
          0xFFFFFFFF80000000ULL ||
      kmsg.msg_iovlen == 0 || kmsg.msg_iovlen > 1024) {
    file_put(sf);
    return (int64_t)-EINVAL;
  }

  // Copy iovec array to kernel
  struct iovec *kiov =
      (struct iovec *)kmalloc(kmsg.msg_iovlen * sizeof(struct iovec));
  if (!kiov) {
    file_put(sf);
    return (int64_t)-ENOMEM;
  }
  if (copy_from_user(kiov, (const void __user *)kmsg.msg_iov,
                     kmsg.msg_iovlen * sizeof(struct iovec))) {
    kfree(kiov);
    file_put(sf);
    return (int64_t)-EFAULT;
  }

  // Validate each iov base pointer
  for (size_t i = 0; i < kmsg.msg_iovlen; i++) {
    if (kiov[i].iov_base) {
      uint64_t base = (int64_t)kiov[i].iov_base;
      if (base >= 0xFFFFFFFF80000000ULL ||
          base + kiov[i].iov_len > 0xFFFFFFFF80000000ULL) {
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
    if (!kcontrol) {
      kfree(kiov);
      file_put(sf);
      return (int64_t)-ENOMEM;
    }
    if (copy_from_user(kcontrol, (const void __user *)kmsg.msg_control,
                       kmsg.msg_controllen)) {
      kfree(kcontrol);
      kfree(kiov);
      file_put(sf);
      return (int64_t)-EFAULT;
    }
    kcontrollen = kmsg.msg_controllen;
  }

  int64_t ret;
  if (sf->type == FD_NETLINK) {
    struct netlink_sock *nlsock = sf->nlsock;
    if (!nlsock) {
      kfree(kiov);
      if (kcontrol)
        kfree(kcontrol);
      file_put(sf);
      return (int64_t)-EBADF;
    }
    // Netlink: ignore control data (SCM_RIGHTS not supported on netlink)
    ret = netlink_sock_sendmsg(nlsock, kiov, kmsg.msg_iovlen, flags);
  } else {
    struct unix_sock *sock = sf->sock;
    if (!sock) {
      kfree(kiov);
      if (kcontrol)
        kfree(kcontrol);
      file_put(sf);
      return (int64_t)-EBADF;
    }
    ret = unix_sock_sendmsg(sock, kiov, kmsg.msg_iovlen, kcontrol, kcontrollen,
                            flags);
  }

  kfree(kiov);
  if (kcontrol)
    kfree(kcontrol);
  file_put(sf);
  return (int64_t)ret;
}

int64_t sys_recvmsg(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                    int64_t unused2, int64_t unused3) {
  int fd = (int)arg1;
  struct msghdr __user *msg = (struct msghdr __user *)arg2;
  int flags = (int)arg3;

  xtask *proc = current_task;

  if (fd < 0 || fd >= MAX_FD)
    return (int64_t)-EBADF;
  rcu_read_lock();
  struct file *rf = fd_lookup(proc->proc->files, fd);
  if (!rf || (rf->type != FD_SOCKET && rf->type != FD_NETLINK)) {
    rcu_read_unlock();
    return (int64_t)-ENOTSOCK;
  }
  file_get(rf);
  rcu_read_unlock();

  uint64_t msg_ptr = (int64_t)msg;
  if (!msg_ptr || msg_ptr >= 0xFFFFFFFF80000000ULL ||
      msg_ptr + sizeof(struct msghdr) > 0xFFFFFFFF80000000ULL) {
    file_put(rf);
    return (int64_t)-EFAULT;
  }

  struct msghdr kmsg;
  if (copy_from_user(&kmsg, msg, sizeof(struct msghdr))) {
    file_put(rf);
    return (int64_t)-EFAULT;
  }

  // Validate iov
  uint64_t iov_ptr = (int64_t)kmsg.msg_iov;
  if (!iov_ptr || iov_ptr >= 0xFFFFFFFF80000000ULL ||
      iov_ptr + kmsg.msg_iovlen * sizeof(struct iovec) >
          0xFFFFFFFF80000000ULL ||
      kmsg.msg_iovlen == 0 || kmsg.msg_iovlen > 1024) {
    file_put(rf);
    return (int64_t)-EINVAL;
  }

  struct iovec *kiov =
      (struct iovec *)kmalloc(kmsg.msg_iovlen * sizeof(struct iovec));
  if (!kiov) {
    file_put(rf);
    return (int64_t)-ENOMEM;
  }
  if (copy_from_user(kiov, (const void __user *)kmsg.msg_iov,
                     kmsg.msg_iovlen * sizeof(struct iovec))) {
    kfree(kiov);
    file_put(rf);
    return (int64_t)-EFAULT;
  }

  for (size_t i = 0; i < kmsg.msg_iovlen; i++) {
    if (kiov[i].iov_base) {
      uint64_t base = (int64_t)kiov[i].iov_base;
      if (base >= 0xFFFFFFFF80000000ULL ||
          base + kiov[i].iov_len > 0xFFFFFFFF80000000ULL) {
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
    kcontrol = (void *)ctrl_ptr; // write directly to user space
    kcontrollen = kmsg.msg_controllen;
  }

  int64_t ret;
  if (rf->type == FD_NETLINK) {
    struct netlink_sock *nlsock = rf->nlsock;
    if (!nlsock) {
      kfree(kiov);
      file_put(rf);
      return (int64_t)-EBADF;
    }
    sockaddr_nl src_addr;
    size_t src_len = sizeof(sockaddr_nl);
    __memset(&src_addr, 0, sizeof(src_addr));
    ret = netlink_sock_recvmsg(nlsock, kiov, kmsg.msg_iovlen, &src_addr,
                               &src_len, flags);
    // Write src_addr to msg_name if requested
    if (ret >= 0 && kmsg.msg_name && kmsg.msg_namelen >= sizeof(sockaddr_nl)) {
      if (copy_to_user((void __user *)kmsg.msg_name, &src_addr,
                       sizeof(sockaddr_nl)))
        ret = (int64_t)-EFAULT;
      else {
        socklen_t out_len = sizeof(sockaddr_nl);
        if (copy_to_user((void __user *)((char __user *)msg + 4), &out_len,
                         sizeof(socklen_t)))
          ret = (int64_t)-EFAULT;
      }
    }
  } else {
    struct unix_sock *sock = rf->sock;
    if (!sock) {
      kfree(kiov);
      file_put(rf);
      return (int64_t)-EBADF;
    }
    ret = unix_sock_recvmsg(sock, kiov, kmsg.msg_iovlen, kcontrol, &kcontrollen,
                            flags);
    // Update msg_controllen in user space (existing code)
    if (ret >= 0) {
      if (kmsg.msg_control && kmsg.msg_controllen > 0) {
        if (copy_to_user((void __user *)((char __user *)msg + 40), &kcontrollen,
                         sizeof(size_t)))
          ret = (int64_t)-EFAULT;
      }
    }
  }

  kfree(kiov);
  file_put(rf);
  return (int64_t)ret;
}

int64_t sys_shutdown(int64_t arg1, int64_t arg2, int64_t unused1,
                     int64_t unused2, int64_t unused3, int64_t unused4) {
  int fd = (int)arg1;
  int how = (int)arg2;

  xtask *proc = current_task;

  if (fd < 0 || fd >= MAX_FD)
    return (int64_t)-EBADF;
  rcu_read_lock();
  struct file *shf = fd_lookup(proc->proc->files, fd);
  if (!shf || shf->type != FD_SOCKET) {
    rcu_read_unlock();
    return (int64_t)-ENOTSOCK;
  }
  file_get(shf);
  rcu_read_unlock();

  struct unix_sock *sock = shf->sock;
  if (!sock) {
    file_put(shf);
    return (int64_t)-EBADF;
  }

  if (how < 0 || how > 2) {
    file_put(shf);
    return (int64_t)-EINVAL;
  }

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
  // If we shut down write, peer's recvmsg should return EOF — wake peer's
  // blocked reader If we shut down read, peer's sendmsg should return EPIPE —
  // wake peer's blocked writer
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

  if (reader >= 0)
    wake_process(reader);
  if (writer >= 0)
    wake_process(writer);
  if (peer_reader >= 0)
    wake_process(peer_reader);
  if (peer_writer >= 0)
    wake_process(peer_writer);

  file_put(shf);
  return 0;
}

// Per-fd wait callback for sys_poll: registered on each polled fd's wq so a
// fd becoming ready (e.g. timerfd expiry) wakes the blocked poll promptly
// instead of waiting out the full deadline. Mirrors eventfd/timerfd wake cbs.
static void poll_wait_cb(wait_queue_t *wq, unsigned long flags) {
  xtask *proc = (xtask *)wq->data;
  (void)flags;
  wake_with_event(proc, WAIT_POLL);
}

int64_t sys_poll(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                 int64_t unused2, int64_t unused3) {
  struct pollfd __user *fds = (struct pollfd __user *)arg1;
  nfds_t nfds = (nfds_t)arg2;
  int timeout_ms = (int)arg3;

  xtask *proc = current_task;

  // Validate user pointer
  uint64_t fds_ptr = (int64_t)fds;
  if (!fds_ptr || fds_ptr >= 0xFFFFFFFF80000000ULL ||
      fds_ptr + nfds * sizeof(struct pollfd) > 0xFFFFFFFF80000000ULL)
    return (int64_t)-EFAULT;

  // Copy pollfd array to kernel
  struct pollfd *kfds = (struct pollfd *)kmalloc(nfds * sizeof(struct pollfd));
  if (!kfds)
    return (int64_t)-ENOMEM;
  if (copy_from_user(kfds, fds, nfds * sizeof(struct pollfd))) {
    kfree(kfds);
    return (int64_t)-EFAULT;
  }

  // Per-fd wait registrations. While blocked, each polled fd holds a reference
  // and a wait_queue_t on its wq so the fd's __wake_up (e.g. timerfd expiry)
  // wakes us instead of waiting out the full deadline.
  //
  // Heap-allocate polled[]/pwq[]: at nfds up to MAX_FD(128) this is ~5KB
  // (128*8 + 128*32). On an 8KB kernel stack that leaves under 1.5KB for the
  // schedule()/switch_to call chain and overruns into pwq, corrupting
  // node->prev to garbage and #PF-ing in list_remove during poll_out teardown.
  // Linux kmallocs its poll_list for the same reason.
  struct file **polled = (struct file **)kmalloc(nfds * sizeof(struct file *));
  wait_queue_t *pwq = (wait_queue_t *)kmalloc(nfds * sizeof(wait_queue_t));
  if (!polled || !pwq) {
    kfree(kfds);
    kfree(polled);
    kfree(pwq);
    return (int64_t)-ENOMEM;
  }
  for (nfds_t i = 0; i < nfds; i++) {
    polled[i] = NULL;
    list_init(&pwq[i].node); // self-ref = "not on any wq"; makes the
                             // on-wq guards below reliable on heap memory
                             // (kmalloc returns poison, not zeroed).
  }

  // polled[i] != NULL marks "we retained a file ref for fd i and its pwq[i] is
  // registered on that fd's wq". poll_out walks it to remove each waiter and
  // drop the ref. Removal must resolve the wq via file_wq_get(f) — the same wq
  // used at add_wait_queue time — not f->wq: for ringbuf-backed FD_DEV the wq
  // is per-inode (f->inode->wq) while f->wq is NULL.
  uint64_t deadline = 0;
  if (timeout_ms > 0) {
    deadline = sched_clock() + (int64_t)timeout_ms * 1000000ULL;
  }

  int64_t ret = 0;
  for (;;) {
    int ready = 0;

    // Check each fd and register a wait entry on its wq.
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
      if (f)
        file_get(f);
      rcu_read_unlock();
      if (!f) {
        kfds[i].revents = POLLERR;
        ready++;
        continue;
      }

      kfds[i].revents |= file_poll(f, kfds[i].events);
      if (kfds[i].revents)
        ready++;

      // Register a wait entry so a later __wake_up on this fd (after this
      // loop's file_put) can wake our BLOCKED poll.
      wait_queue_head *wq = file_wq_get(f);
      if (wq) {
        // A node is "on a wq" iff not self-referential (list_init/list_remove
        // both leave node.{prev,next} == &node). On the first iteration pwq[i]
        // is freshly list_init'd below; on later iterations it may still be
        // registered from the previous pass (we leave waiters in place across
        // the blocked schedule() so wakeups during the wait reach us).
        // Re-adding an already-linked node would corrupt the wq list
        // (list_push_back on a linked node leaves dangling prev->next pointers,
        // which then manifests as self-ref nodes wedged onto the wq and trip
        // remove_wait_queue's WARN + __wake_up's WARN_ON_ONCE). So add at most
        // once per call.
        if (pwq[i].node.next == &pwq[i].node) {
          pwq[i].func = poll_wait_cb;
          pwq[i].data = proc;
          list_init(&pwq[i].node);
          add_wait_queue(wq, &pwq[i]);
        }
        // Drop a ref retained by a previous iteration's fd_lookup for this pfd
        // before overwriting it, so only the latest lookup's ref survives to
        // poll_out's file_put. (Each pass re-looks-up the fd; without this the
        // earlier ref leaks.)
        if (polled[i])
          file_put(polled[i]);
        polled[i] = f; // retain reference until after schedule()
        continue;
      }
      file_put(f);
    }

    if (ready > 0) {
      // Copy results back to user
      if (copy_to_user(fds, kfds, nfds * sizeof(struct pollfd))) {
        ret = (int64_t)-EFAULT;
        goto poll_out;
      }
      ret = (int64_t)ready;
      goto poll_out;
    }

    if (timeout_ms == 0) {
      if (copy_to_user(fds, kfds, nfds * sizeof(struct pollfd))) {
        ret = (int64_t)-EFAULT;
        goto poll_out;
      }
      ret = 0;
      goto poll_out;
    }

    // Block on WAIT_POLL
    if (deadline > 0) {
      uint64_t now = sched_clock();
      if (now >= deadline) {
        __memset(kfds, 0, nfds * sizeof(struct pollfd));
        if (copy_to_user(fds, kfds, nfds * sizeof(struct pollfd))) {
          ret = (int64_t)-EFAULT;
          goto poll_out;
        }
        ret = 0; // timeout
        goto poll_out;
      }
      proc->wait_deadline = deadline;
      uint64_t pflags;
      spin_lock_irqsave(&cpu_locals[proc->assigned_cpu].scheduler_lock,
                        &pflags);
      sched_timer_queue_insert(proc->assigned_cpu, proc);
      spin_unlock_irqrestore(&cpu_locals[proc->assigned_cpu].scheduler_lock,
                             pflags);
    } else {
      proc->wait_deadline = 0;
    }

    proc->state = BLOCKED;
    proc->wait_event = WAIT_POLL;
    proc->wait_timed_out = 0;
    schedule();

    // EINTR check (before timeout check: signal priority over timeout)
    {
      uint64_t pend =
          __atomic_load_n(&proc->proc->sig_pending, __ATOMIC_ACQUIRE);
      uint64_t deliv = pend & ~proc->proc->sig_blocked;
      deliv |= (pend & ((1ULL << SIGKILL) | (1ULL << SIGSTOP)));
      if (deliv) {
        ret = (int64_t)-EINTR;
        goto poll_out;
      }
    }

    if (proc->wait_timed_out && timeout_ms > 0) {
      __memset(kfds, 0, nfds * sizeof(struct pollfd));
      if (copy_to_user(fds, kfds, nfds * sizeof(struct pollfd))) {
        ret = (int64_t)-EFAULT;
        goto poll_out;
      }
      ret = 0; // timeout
      goto poll_out;
    }

    // Woken up — re-check all fds
  }

poll_out:
  // Tear down every wait registration still held. Single cleanup point so no
  // return path can leave a pwq[i] on a wq (leaked heap node + dangling wq
  // links that wedge a later __wake_up traversal).
  for (nfds_t i = 0; i < nfds; i++) {
    if (!polled[i])
      continue;
    struct file *f = polled[i];
    // Only remove a node that is actually on a wq (not self-referential). A
    // node we registered is linked here; guarding avoids remove_wait_queue's
    // self-ref WARN if a future path skips the add, and avoids a needless
    // file_wq_get that would lazily allocate a wq just to remove from it.
    if (pwq[i].node.next != &pwq[i].node) {
      wait_queue_head *wq = file_wq_get(f);
      if (wq)
        remove_wait_queue(wq, &pwq[i]);
    }
    file_put(f);
    polled[i] = NULL;
  }

  kfree(kfds);
  kfree(polled);
  kfree(pwq);
  return ret;
}

// ===================== unix_sock_write / unix_sock_read (for
// sys_write/sys_read FD_SOCKET dispatch) =====================

int64_t unix_sock_write(struct unix_sock *sock, const void *buf, size_t len) {
  struct iovec iov;
  iov.iov_base = (void *)buf;
  iov.iov_len = len;
  return unix_sock_sendmsg(sock, &iov, 1, NULL, 0, 0);
}

int64_t unix_sock_read(struct unix_sock *sock, void *buf, size_t len) {
  struct iovec iov;
  iov.iov_base = buf;
  iov.iov_len = len;
  return unix_sock_recvmsg(sock, &iov, 1, NULL, NULL, 0);
}

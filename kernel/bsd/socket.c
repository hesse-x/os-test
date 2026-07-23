/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arch/x64/apic.h"
#include "arch/x64/memlayout.h" // KERNEL_VMA_BOUNDARY
#include "arch/x64/smp.h"
#include "arch/x64/utils.h"
#include "kernel/bsd/file_poll.h"
#include "kernel/bsd/inode.h"
#include "kernel/bsd/mount.h"
#include "kernel/bsd/netlink.h"
#include "kernel/bsd/proc.h"
#include "kernel/bsd/signal.h" // signal_struct (alarm_deadline / sig_lock)
#include "kernel/bsd/socket.h"
#include "kernel/bsd/syscall.h" // deliver_signal_to, SIGPIPE
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

// Forward declaration: poll_wait_cb is defined later (per-fd poll callback)
// but used by unix_sock_recvmsg/sys_accept before its definition.
static void poll_wait_cb(wait_queue_t *wq, unsigned long flags);

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
  /* S08: 权限位应用 umask,设 owner=创建进程。 */
  int eff_mode = S_IFSOCK | (0777 & ~(int)current_proc->umask);
  struct inode *ip = parent->i_op->create(parent, lastname, eff_mode);
  inode_put(parent);
  if (IS_ERR(ip))
    return ip;
  ip->uid = current_proc->uid;
  ip->gid = current_proc->gid;
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

/* DGRAM lookup: a bound SOCK_DGRAM socket is reachable by path. Unlike
 * unix_bind_lookup (which requires UNIX_LISTEN), a DGRAM target is valid in
 * UNIX_DGRAM_BOUND or UNIX_CONNECTED. Caller holds socket_lock; the returned
 * pointer is borrowed for the locked section only. */
int unix_bind_lookup_dgram(const char *sun_path, struct unix_sock **out) {
  struct inode *ip = vfs_lookup_socket(sun_path);
  if (!IS_ERR(ip) && ip) {
    struct unix_sock *s = (struct unix_sock *)ip->i_priv;
    if (s && s->type == SOCK_DGRAM &&
        (s->state == UNIX_DGRAM_BOUND || s->state == UNIX_CONNECTED)) {
      *out = s;
      inode_put(ip);
      return 0;
    }
    inode_put(ip);
    if (s && s->type != SOCK_DGRAM)
      return -EPROTOTYPE; // path bound to a STREAM socket
    return -ECONNREFUSED; // socket inode exists but not a bound DGRAM
  }
  // Hash-table fallback (VFS path failed to resolve).
  uint32_t h = unix_hash(sun_path);
  for (struct unix_bind_entry *e = unix_bind_table[h]; e; e = e->next) {
    int i = 0;
    while (i < 108 && sun_path[i] == e->sun_path[i]) {
      if (sun_path[i] == '\0')
        break;
      i++;
    }
    if (i < 108 && sun_path[i] == '\0' && e->sun_path[i] == '\0') {
      if (e->sock && e->sock->type == SOCK_DGRAM &&
          (e->sock->state == UNIX_DGRAM_BOUND ||
           e->sock->state == UNIX_CONNECTED)) {
        *out = e->sock;
        return 0;
      }
      if (e->sock && e->sock->type != SOCK_DGRAM)
        return -EPROTOTYPE;
      return -ECONNREFUSED;
    }
  }
  return -ENOENT;
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
  skb->has_sender = 0;
  __memset(&skb->sender_addr, 0, sizeof(skb->sender_addr));
  return skb;
}

void skb_free(struct sk_buff *skb) {
  if (!skb)
    return;
  // Release any SCM_RIGHTS file refs the skb still owns. The recv path
  // transfers ownership (clears num_fds) when it installs the files; this
  // covers dropped/flushed skbs (e.g. socket close with unconsumed data).
  for (int i = 0; i < skb->num_fds; i++) {
    if (skb->files[i])
      file_put(skb->files[i]);
  }
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
  sock->type = SOCK_STREAM;
  sock->peer = -1;
  sock->peer_sock = NULL;
  refcount_set(&sock->u_count, 1);
  sock->recv_queue_head = NULL;
  sock->recv_queue_tail = NULL;
  sock->recv_queue_len = 0;
  sock->backlog_head = NULL;
  sock->backlog_tail = NULL;
  sock->backlog_len = 0;
  sock->backlog_max = 0;
  sock->shutdown_read = 0;
  sock->shutdown_write = 0;
  sock->sun_path[0] = '\0';
  sock->dgram_dst_path[0] = '\0';
  sock->bind_inode = NULL;
  sock->owner_pid = -1;
  /* eager 分配 wq：阻塞 reader 改 add_wait_queue 后 wq 必须在创建时非
   * NULL（§7.1）。 */
  sock->wq = (wait_queue_head *)kmalloc(sizeof(wait_queue_head));
  if (!sock->wq) {
    kfree(sock);
    return NULL;
  }
  init_wait_queue_head(sock->wq);
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
  __wake_up(sock->wq, POLLIN);
}

void unix_sock_wake_writer(struct unix_sock *sock) {
  __wake_up(sock->wq, POLLOUT);
}

// Fill the per-skb sender address from a bound DGRAM socket. has_sender is 0
// for an anonymous (unbound) sender; recvmsg then reports only the family.
static void unix_dgram_fill_sender(struct sk_buff *skb, struct unix_sock *src) {
  skb->sender_addr.sun_family = AF_UNIX;
  __memcpy(skb->sender_addr.sun_path, src->sun_path, UNIX_PATH_MAX);
  skb->has_sender = (src->sun_path[0] != '\0');
}

// Forward decl: unix_scm_resolve is defined later (before unix_sock_sendmsg)
// but used by unix_dgram_sendto here.
static int unix_scm_resolve(struct sk_buff *skb, const void *control,
                            size_t controllen);

// DGRAM sendto: resolve dest path to a bound DGRAM socket, enqueue one whole
// skb carrying the sender's address. The target's u_count is bumped under
// socket_lock so the post-unlock wake cannot dereference a freed socket.
int64_t unix_dgram_sendto(struct unix_sock *src, const struct sockaddr_un *dest,
                          const struct iovec *iov, size_t iovlen,
                          const void *control, size_t controllen, int flags) {
  if (dest->sun_family != AF_UNIX)
    return -EAFNOSUPPORT;
  if (dest->sun_path[0] == '\0')
    return -EINVAL;

  // Total payload length and overflow check.
  uint32_t total = 0;
  for (size_t i = 0; i < iovlen; i++)
    total += (uint32_t)iov[i].iov_len;
  if (total > MAX_SOCKET_DATA)
    return -EMSGSIZE;

  struct sk_buff *skb = skb_alloc(total);
  if (!skb)
    return -ENOMEM;
  uint32_t offset = 0;
  for (size_t i = 0; i < iovlen; i++) {
    if (iov[i].iov_base && iov[i].iov_len > 0) {
      if (copy_from_user(skb->data + offset,
                         (const void __user *)iov[i].iov_base,
                         iov[i].iov_len)) {
        skb_free(skb);
        return -EFAULT;
      }
      offset += (uint32_t)iov[i].iov_len;
    }
  }
  int sret = unix_scm_resolve(skb, control, controllen);
  if (sret) {
    skb_free(skb);
    return sret;
  }
  unix_dgram_fill_sender(skb, src);

  spin_lock(&socket_lock);
  struct unix_sock *target = NULL;
  int ret = unix_bind_lookup_dgram(dest->sun_path, &target);
  if (ret) {
    spin_unlock(&socket_lock);
    skb_free(skb);
    return ret;
  }
  if (target->shutdown_read) {
    spin_unlock(&socket_lock);
    skb_free(skb);
    return -ECONNREFUSED;
  }
  if ((flags & MSG_DONTWAIT) && target->recv_queue_len > 128) {
    spin_unlock(&socket_lock);
    skb_free(skb);
    return -EAGAIN;
  }
  refcount_inc(&target->u_count);
  skb_enqueue(target, skb);
  wait_queue_head *twq = target->wq;
  spin_unlock(&socket_lock);

  __wake_up(twq, POLLIN);
  unix_sock_release(target);
  return (int64_t)total;
}

// DGRAM recvmsg: read exactly one skb (preserving message boundaries), backfill
// the sender address, and install SCM_RIGHTS. MSG_PEEK leaves the skb queued;
// MSG_TRUNC reports the true datagram length. MSG_WAITALL is a no-op for
// datagrams (a datagram cannot be coalesced across messages). Mirrors the
// STREAM recvmsg blocking/EOFEINTR/SCM-install/cmsg-writeback structure.
int64_t unix_dgram_recvmsg(struct unix_sock *sock, const struct iovec *iov,
                           size_t iovlen, void *control, size_t *controllen,
                           int flags, int *out_flags,
                           struct sockaddr_un *sender_addr,
                           size_t *sender_len) {
  bool nonblock = (flags & MSG_DONTWAIT) != 0;
  bool peek = (flags & MSG_PEEK) != 0;

  for (;;) {
    spin_lock(&socket_lock);
    struct sk_buff *skb = sock->recv_queue_head;

    if (!skb) {
      // Empty queue. A DGRAM socket has no peer requirement (multiple senders
      // may target one bound socket); only shutdown_read signals EOF.
      if (sock->shutdown_read) {
        spin_unlock(&socket_lock);
        if (sender_len)
          *sender_len = 0;
        return 0;
      }
      if (nonblock) {
        spin_unlock(&socket_lock);
        return -EAGAIN;
      }
      // Block on sock->wq (same pattern as unix_sock_recvmsg).
      xtask *proc = current_task;
      wait_queue_t wait;
      wait.func = poll_wait_cb;
      wait.data = proc;
      wait.exclusive = 0;
      list_init(&wait.node);
      add_wait_queue(sock->wq, &wait);
      proc->state = BLOCKED;
      proc->wait_event = WAIT_POLL;
      proc->wait_deadline = sched_clock() + 30000000000ULL; // 30s
      spin_unlock(&socket_lock);
      int cpu = proc->assigned_cpu;
      uint64_t rflags;
      spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &rflags);
      sched_timer_queue_insert(cpu, proc);
      spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, rflags);
      schedule();
      if (proc->wait_timed_out) {
        proc->state = RUNNING;
        remove_wait_queue(sock->wq, &wait);
        return -ETIMEDOUT;
      }
      {
        uint64_t pend =
            __atomic_load_n(&proc->proc->sig_pending, __ATOMIC_ACQUIRE);
        uint64_t deliv = pend & ~proc->proc->sig_blocked;
        deliv |= (pend & ((SIGMASK(SIGKILL)) | (SIGMASK(SIGSTOP))));
        if (deliv) {
          proc->state = RUNNING;
          remove_wait_queue(sock->wq, &wait);
          return -EINTR;
        }
      }
      proc->state = RUNNING;
      remove_wait_queue(sock->wq, &wait);
      continue;
    }

    // One whole datagram: copy min(iov capacity, skb->len). No partial-read
    // cursor (consumed stays 0); a too-small buffer truncates and sets
    // MSG_TRUNC.
    uint32_t total = skb->len;
    uint32_t iov_cap = 0;
    for (size_t i = 0; i < iovlen; i++)
      iov_cap += (uint32_t)iov[i].iov_len;
    uint32_t to_read = iov_cap < total ? iov_cap : total;

    uint32_t off = 0, rem = to_read;
    for (size_t i = 0; i < iovlen && rem > 0; i++) {
      if (iov[i].iov_base && iov[i].iov_len > 0) {
        uint32_t c = (uint32_t)iov[i].iov_len;
        if (c > rem)
          c = rem;
        if (copy_to_user((void __user *)iov[i].iov_base, skb->data + off, c)) {
          spin_unlock(&socket_lock);
          return -EFAULT;
        }
        off += c;
        rem -= c;
      }
    }

    // Backfill sender address.
    if (sender_addr && sender_len) {
      *sender_addr = skb->sender_addr;
      *sender_len =
          skb->has_sender ? sizeof(struct sockaddr_un) : sizeof(sa_family_t);
    }

    bool truncated = (total > iov_cap);

    if (peek) {
      // PEEK: do not consume the skb or transfer SCM_RIGHTS.
      spin_unlock(&socket_lock);
      if (control && controllen)
        *controllen = 0;
      if (truncated && out_flags)
        *out_flags |= MSG_TRUNC;
      // With MSG_TRUNC the caller learns the true datagram length; otherwise
      // report the bytes actually visible (== to_read).
      if ((flags & MSG_TRUNC) && truncated)
        return (int64_t)total;
      return (int64_t)to_read;
    }

    // Extract SCM_RIGHTS refs before releasing socket_lock (same ownership
    // transfer as the STREAM path).
    int num_fds_to_install = 0;
    struct file *scm_files[SCM_MAX_FD];
    if (skb->num_fds > 0) {
      for (int i = 0; i < skb->num_fds && num_fds_to_install < SCM_MAX_FD; i++)
        scm_files[num_fds_to_install++] = skb->files[i];
      skb->num_fds = 0;
    }
    bool do_scm = (num_fds_to_install > 0);

    skb_dequeue(sock);
    skb_free(skb);
    spin_unlock(&socket_lock);

    // Install SCM_RIGHTS files into the receiver's fd table.
    int installed_fds[SCM_MAX_FD];
    int num_fds_installed = 0;
    bool scm_truncated = false;
    if (do_scm) {
      xtask *proc = current_task;
      for (int i = 0; i < num_fds_to_install && num_fds_installed < SCM_MAX_FD;
           i++) {
        struct file *src = scm_files[i];
        spinlock *fdlk = &proc->proc->files->fd_lock;
        spin_lock(fdlk);
        int new_fd = alloc_fd(proc->proc->files, 0);
        if (new_fd < 0) {
          spin_unlock(fdlk);
          file_put(src);
          scm_truncated = true;
          break;
        }
        fd_install(proc->proc->files, new_fd, src);
        if (src->type == FD_TTY)
          pty_dup_file(src);
        spin_unlock(fdlk);
        installed_fds[num_fds_installed++] = new_fd;
      }
      for (int i = num_fds_installed; i < num_fds_to_install; i++)
        file_put(scm_files[i]);
    }

    // Write SCM_RIGHTS cmsg back to the control buffer.
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
        for (int i = 0; i < num_fds_installed; i++)
          out_fds[i] = installed_fds[i];
        *controllen = cmsg->cmsg_len;
      } else {
        *controllen = 0;
        if (out_flags)
          *out_flags |= MSG_CTRUNC;
      }
    } else if (control && controllen) {
      *controllen = 0;
    }
    if (scm_truncated && out_flags)
      *out_flags |= MSG_CTRUNC;

    if (truncated && out_flags)
      *out_flags |= MSG_TRUNC;

    // With MSG_TRUNC a truncated datagram reports its true length; otherwise
    // the bytes actually copied (== to_read).
    if ((flags & MSG_TRUNC) && truncated)
      return (int64_t)total;
    return (int64_t)to_read;
  }
}

// ===================== Internal sendmsg/recvmsg =====================

// Resolve SCM_RIGHTS cmsgs in the control buffer into ref-held struct file*
// stored on the skb (skb->files[]/num_fds). Resolves while the sender's fd
// table is authoritative; file_get() keeps each file alive until the receiver
// installs it (or skb_free drops the ref). Returns 0/-EINVAL/-EBADF.
static int unix_scm_resolve(struct sk_buff *skb, const void *control,
                            size_t controllen) {
  if (!control || controllen < sizeof(struct cmsghdr))
    return 0;
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
      if (num_fds > SCM_MAX_FD)
        return -EINVAL;
      if (num_fds > 0) {
        int *fds = (int *)CMSG_DATA(cmsg);
        rcu_read_lock();
        for (int i = 0; i < num_fds; i++) {
          if (fds[i] < 0 || fds[i] >= MAX_FD) {
            rcu_read_unlock();
            return -EBADF;
          }
          struct file *tf = fd_lookup(current_proc->files, fds[i]);
          if (!tf) {
            rcu_read_unlock();
            return -EBADF;
          }
          file_get(tf);
          skb->files[skb->num_fds++] = tf;
        }
        rcu_read_unlock();
      }
    } else {
      return -EINVAL;
    }
    cmsg_ptr += aligned_len;
  }
  return 0;
}

int64_t unix_sock_sendmsg(struct unix_sock *sock, const struct iovec *iov,
                          size_t iovlen, const void *control, size_t controllen,
                          int flags) {
  // DGRAM 已连接（connect() 缓存了目标 path）：无 addr 的 send/sendmsg/write
  // 按 dgram_dst_path 动态查找目标，走 unix_dgram_sendto（它对 target 取
  // 临时 u_count 引用，wake-after-unlock 安全）。DGRAM 不持有持久 peer_sock
  // 指针，避免对端 close 后本端 peer_sock 悬空 UAF。带 addr 的 DGRAM 发送
  // 在 sys_sendmsg/sys_sendto 入口已直接走 unix_dgram_sendto，不会到此。
  if (sock->type == SOCK_DGRAM && sock->dgram_dst_path[0] != '\0') {
    struct sockaddr_un dest;
    __memset(&dest, 0, sizeof(dest));
    dest.sun_family = AF_UNIX;
    __memcpy(dest.sun_path, sock->dgram_dst_path, sizeof(dest.sun_path));
    dest.sun_path[sizeof(dest.sun_path) - 1] = '\0';
    return unix_dgram_sendto(sock, &dest, iov, iovlen, control, controllen,
                             flags);
  }

  // Check shutdown_write on our socket
  if (sock->shutdown_write) {
    if (!(flags & MSG_NOSIGNAL))
      deliver_signal_to(current_task, SIGPIPE);
    return -EPIPE;
  }
  // Check peer shutdown_read. STREAM sends to a shut-read peer → EPIPE
  // (+SIGPIPE unless MSG_NOSIGNAL); DGRAM → ECONNREFUSED (no SIGPIPE: a
  // connectionless socket never raises SIGPIPE on a refused peer).
  if (sock->peer_sock && sock->peer_sock->shutdown_read) {
    if (sock->type == SOCK_DGRAM)
      return -ECONNREFUSED;
    if (!(flags & MSG_NOSIGNAL))
      deliver_signal_to(current_task, SIGPIPE);
    return -EPIPE;
  }

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

  // Resolve SCM_RIGHTS control messages into ref-held struct file* on the skb.
  // Shared by STREAM and DGRAM send paths. On failure the caller must skb_free.
  int sret = unix_scm_resolve(skb, control, controllen);
  if (sret) {
    skb_free(skb);
    return sret;
  }

  // Find peer socket via direct pointer (socketpair/connect) or PID-based
  // lookup (connect)
  struct unix_sock *peer_sock = sock->peer_sock;
  if (!peer_sock) {
    // STREAM: peer_sock is always set during connect/socketpair — unreachable.
    // DGRAM: an unconnected DGRAM with no dest addr must not reach here (the
    // sendto+addr path uses unix_dgram_sendto); a bare send/sendmsg on an
    // unconnected DGRAM is ENOTCONN.
    if (sock->type == SOCK_STREAM) {
      WARN_ON(1);
      skb_free(skb);
      if (!(flags & MSG_NOSIGNAL))
        deliver_signal_to(current_task, SIGPIPE);
      return -EPIPE;
    }
    skb_free(skb);
    return -ENOTCONN;
  }

  // Check peer shutdown (under socket_lock)
  spin_lock(&socket_lock);
  if (peer_sock->shutdown_read) {
    spin_unlock(&socket_lock);
    skb_free(skb);
    if (sock->type == SOCK_DGRAM)
      return -ECONNREFUSED;
    if (!(flags & MSG_NOSIGNAL))
      deliver_signal_to(current_task, SIGPIPE);
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
  // DGRAM: stamp the sender address onto the skb so the receiver's recvmsg/
  // recvfrom can report msg_name. STREAM leaves has_sender=0 (no name).
  if (sock->type == SOCK_DGRAM)
    unix_dgram_fill_sender(skb, sock);
  spin_unlock(&socket_lock);

  // Wake reader (挂 peer_sock->wq) outside socket_lock
  __wake_up(peer_sock->wq, POLLIN);

  return (int64_t)total;
}

int64_t unix_sock_recvmsg(struct unix_sock *sock, const struct iovec *iov,
                          size_t iovlen, void *control, size_t *controllen,
                          int flags, int *out_flags) {
  bool nonblock = (flags & MSG_DONTWAIT) != 0;
  bool peek = (flags & MSG_PEEK) != 0;
  bool waitall = (flags & MSG_WAITALL) != 0 && !nonblock;

  // Writable iov copy so MSG_WAITALL can advance iov_base/iov_len across
  // multiple skb deliveries without mutating the caller's array. sendmsg uses
  // the same kiov pattern (symmetry). For the common single-recv path this is
  // one small allocation.
  struct iovec *kiov = (struct iovec *)kmalloc(iovlen * sizeof(struct iovec));
  if (!kiov)
    return -ENOMEM;
  for (size_t i = 0; i < iovlen; i++)
    kiov[i] = iov[i];

  size_t total_want = 0; // bytes still requested across all iovs
  for (size_t i = 0; i < iovlen; i++)
    total_want += kiov[i].iov_len;

  size_t total_filled = 0; // bytes filled so far (WAITALL accumulation)
  // Length of the most recently read skb (full skb->len, independent of how
  // much fit in the buffer). Used for MSG_TRUNC's DGRAM-style return value
  // (report the true datagram/record length even when the buffer was smaller).
  uint32_t last_skb_len = 0;

  for (;;) {
    spin_lock(&socket_lock);

    // Check recv queue
    struct sk_buff *skb = sock->recv_queue_head;

    if (!skb) {
      // Queue empty: check EOF conditions
      // 1) Our own shutdown_read
      if (sock->shutdown_read) {
        spin_unlock(&socket_lock);
        break; // EOF
      }
      // 2) Peer shutdown_write (we will never receive more data)
      if (sock->peer_sock && sock->peer_sock->shutdown_write) {
        spin_unlock(&socket_lock);
        break; // EOF — peer shut down write side
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
          break; // EOF
        }
      }

      // MSG_WAITALL with partial data already filled: if the peer hit EOF
      // mid-fill, fall through to return what we have (Linux: short read on
      // EOF). Detected above via the break paths; reaching here means the
      // queue is still empty but not EOF — block (or EAGAIN/EINTR).
      if (nonblock) {
        spin_unlock(&socket_lock);
        kfree(kiov);
        if (total_filled > 0)
          return (int64_t)total_filled;
        return -EAGAIN;
      }

      // Block reader: 持 socket_lock 挂 wq（模式2，条件检查与挂 wq
      // 同一临界区防丢唤醒）。
      xtask *proc = current_task;
      wait_queue_t wait;
      wait.func = poll_wait_cb;
      wait.data = proc;
      wait.exclusive = 0;
      list_init(&wait.node);
      add_wait_queue(sock->wq, &wait);
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
        proc->state = RUNNING;
        remove_wait_queue(sock->wq, &wait);
        kfree(kiov);
        if (total_filled > 0)
          return (int64_t)total_filled;
        return -ETIMEDOUT;
      }
      // EINTR check
      {
        uint64_t pend =
            __atomic_load_n(&proc->proc->sig_pending, __ATOMIC_ACQUIRE);
        uint64_t deliv = pend & ~proc->proc->sig_blocked;
        deliv |= (pend & ((SIGMASK(SIGKILL)) | (SIGMASK(SIGSTOP))));
        if (deliv) {
          proc->state = RUNNING;
          remove_wait_queue(sock->wq, &wait);
          kfree(kiov);
          // Linux MSG_WAITALL: a signal interrupting a partial read returns
          // the bytes already copied; with nothing copied, return -EINTR.
          if (total_filled > 0)
            return (int64_t)total_filled;
          return (int64_t)-EINTR;
        }
      }
      proc->state = RUNNING;
      remove_wait_queue(sock->wq, &wait);
      continue; // 醒后回顶重新 lock → 重查 recv_queue
    }

    // We have data. Calculate how much to read into the remaining iov space.
    last_skb_len = skb->len;
    uint32_t avail = skb->len - skb->consumed;
    uint32_t to_read = 0;
    for (size_t i = 0; i < iovlen && to_read < avail; i++) {
      uint32_t copy = (uint32_t)kiov[i].iov_len;
      if (copy > avail - to_read)
        copy = avail - to_read;
      to_read += copy;
    }

    // Copy data to iovecs (from the writable kiov, which carries the
    // per-iov offset advanced by prior WAITALL iterations).
    uint32_t data_offset = skb->consumed;
    uint32_t remaining = to_read;
    for (size_t i = 0; i < iovlen && remaining > 0; i++) {
      if (kiov[i].iov_base && kiov[i].iov_len > 0) {
        uint32_t copy = (uint32_t)kiov[i].iov_len;
        if (copy > remaining)
          copy = remaining;
        if (copy_to_user((void __user *)kiov[i].iov_base,
                         skb->data + data_offset, copy)) {
          spin_unlock(&socket_lock);
          kfree(kiov);
          return -EFAULT;
        }
        data_offset += copy;
        remaining -= copy;
      }
    }

    // MSG_PEEK: do not consume the skb. Data stays in the queue for the next
    // recv; ancillary (SCM_RIGHTS) fds are not transferred on PEEK (Linux:
    // PEEK leaves ancillary data in the queue for a subsequent normal recv).
    // The reported byte count is normally `to_read` (PEEK returns the same
    // count as a normal read, it just doesn't advance). MSG_TRUNC|PEEK
    // returns the full skb length instead.
    if (peek) {
      spin_unlock(&socket_lock);
      if (control && controllen)
        *controllen = 0;
      kfree(kiov);
      if (flags & MSG_TRUNC) {
        if (out_flags && last_skb_len > to_read)
          *out_flags |= MSG_TRUNC;
        return (int64_t)last_skb_len;
      }
      return (int64_t)to_read;
    }

    skb->consumed += to_read;
    bool skb_consumed = (skb->consumed >= skb->len);

    // Extract SCM_RIGHTS file refs from skb before releasing socket_lock.
    // Ownership transfers to scm_files[] (we clear skb->num_fds so skb_free
    // won't drop the refs); the install loop below owns them thereafter.
    int num_fds_to_install = 0;
    struct file *scm_files[SCM_MAX_FD];
    if (skb->num_fds > 0 && skb_consumed) {
      for (int i = 0; i < skb->num_fds && num_fds_to_install < SCM_MAX_FD;
           i++) {
        scm_files[num_fds_to_install++] = skb->files[i];
      }
      skb->num_fds = 0; // transferred ownership
    }

    // Remove consumed skb and release socket_lock
    bool do_scm = (num_fds_to_install > 0);
    if (skb_consumed) {
      skb_dequeue(sock);
      skb_free(skb);
    }

    spin_unlock(&socket_lock);

    // Install SCM_RIGHTS files into the receiver's fd table — sequential
    // locks, no nesting. Files were ref-resolved at sendmsg time, so no
    // sender pid/fd re-lookup (which previously broke socket-activation: the
    // listening fd is bound by init but accept/sendmsg run in udevd).
    int installed_fds[SCM_MAX_FD];
    int num_fds_installed = 0;
    bool scm_truncated = false; // EMFILE: fewer installed than requested
    if (do_scm) {
      xtask *proc = current_task;
      for (int i = 0; i < num_fds_to_install && num_fds_installed < SCM_MAX_FD;
           i++) {
        struct file *src = scm_files[i];

        // receiver_fd_lock — find free slot + install pointer
        spinlock *fdlk = &proc->proc->files->fd_lock;
        spin_lock(fdlk);
        int new_fd = alloc_fd(proc->proc->files, 0);
        if (new_fd < 0) {
          spin_unlock(fdlk);
          file_put(src); // drop this file's ref
          scm_truncated = true;
          break; // no free fd slots
        }
        fd_install(proc->proc->files, new_fd,
                   src); // install pointer directly, no extra bump needed
        if (src->type == FD_TTY)
          pty_dup_file(src);
        spin_unlock(fdlk);

        installed_fds[num_fds_installed++] = new_fd;
      }
      // Drop refs for any files we didn't install (e.g. broke out early on
      // EMFILE). Installed files are now owned by the receiver's fd table.
      for (int i = num_fds_installed; i < num_fds_to_install; i++)
        file_put(scm_files[i]);
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
        // Control buffer too small for even one fd: report truncation. The
        // fds are still installed in the receiver's table (consumed); only the
        // ancillary report is lost. Linux sets MSG_CTRUNC and zeroes
        // msg_controllen.
        *controllen = 0;
        if (out_flags)
          *out_flags |= MSG_CTRUNC;
      }
    } else if (control && controllen) {
      *controllen = 0;
    }
    // EMFILE mid-install: fewer fds installed than the skb carried. The
    // receiver got a partial set; signal that ancillary data was truncated.
    if (scm_truncated && out_flags)
      *out_flags |= MSG_CTRUNC;

    // Advance the writable iov copy by the bytes just filled, so a subsequent
    // WAITALL iteration continues filling where this one left off.
    {
      uint32_t adv = to_read;
      for (size_t i = 0; i < iovlen && adv > 0; i++) {
        if (kiov[i].iov_len <= adv) {
          adv -= (uint32_t)kiov[i].iov_len;
          kiov[i].iov_len = 0;
        } else {
          kiov[i].iov_base =
              (void *)((char *)(__force uintptr_t)kiov[i].iov_base + adv);
          kiov[i].iov_len -= adv;
          adv = 0;
        }
      }
    }
    total_filled += to_read;

    if (!waitall || total_filled >= total_want)
      break; // request satisfied (or single-recv semantics)
    // WAITALL: loop to fetch the next skb (blocking again at queue head).
  }

  kfree(kiov);

  // MSG_TRUNC (DGRAM-style semantics for AF_UNIX STREAM, per S16 design):
  // the return value is the true length of the datagram/record on the queue
  // (the full skb), even when the user buffer was smaller and only part was
  // copied. The buffer is still filled with whatever fit. Flag MSG_TRUNC so
  // the caller knows the record was larger than the buffer. For WAITALL the
  // relevant record is the final skb read (last_skb_len); without WAITALL a
  // single recv reads exactly one skb, so last_skb_len is that skb's length.
  if (flags & MSG_TRUNC) {
    if (out_flags && last_skb_len > total_filled)
      *out_flags |= MSG_TRUNC;
    return (int64_t)last_skb_len;
  }

  return (int64_t)total_filled;
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

  // Notify peer: mark its read side shutdown + detach back-reference
  struct unix_sock *peer_s = sock->peer_sock;
  if (peer_s) {
    // Mark the peer's read side as shutdown so subsequent recv/poll on the
    // peer sees EOF (POLLIN with read returning 0), matching Linux semantics
    // for close() of the remote end.
    peer_s->shutdown_read = 1;
    // Detach back-reference so the peer never dereferences us after free.
    peer_s->peer_sock = NULL;
  }

  spin_unlock(&socket_lock);

  // 唤醒本端与对端阻塞 reader/writer（各自挂自己 wq）+ epoll 等待者（POLLHUP）
  __wake_up(sock->wq, POLLHUP | POLLIN | POLLOUT);
  if (peer_s)
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
    f->nlsock = nl;
    fd_install(proc->proc->files, fd, f);
    fd_set_cloexec(proc->proc->files, fd, (sock_flags & SOCK_CLOEXEC) ? 1 : 0);

    spin_unlock(fdlk);
    return (int64_t)fd;
  }

  if (domain != AF_UNIX)
    return (int64_t)-EAFNOSUPPORT;
  if (base_type != SOCK_STREAM && base_type != SOCK_DGRAM)
    return (int64_t)-EPROTONOSUPPORT;
  if (protocol != 0)
    return (int64_t)-EPROTONOSUPPORT;

  struct unix_sock *sock = unix_sock_alloc();
  if (!sock)
    return (int64_t)-ENOMEM;

  xtask *proc = current_task;

  sock->state = UNIX_FREE;
  sock->type = base_type;

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
  f->sock = sock;
  fd_install(proc->proc->files, fd, f);
  fd_set_cloexec(proc->proc->files, fd, (sock_flags & SOCK_CLOEXEC) ? 1 : 0);

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
  if (!addr_ptr || addr_ptr >= KERNEL_VMA_BOUNDARY ||
      addr_ptr + addrlen > KERNEL_VMA_BOUNDARY || addrlen <= 0) {
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

  // STREAM may bind while UNIX_FREE (listen transitions to UNIX_LISTEN). A
  // DGRAM socket binds straight into UNIX_DGRAM_BOUND. Both start from
  // UNIX_FREE here; UNIX_DGRAM_BOUND is rejected so a second bind fails.
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

  if (sock->type == SOCK_DGRAM)
    sock->state = UNIX_DGRAM_BOUND;

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
    if (aptr >= KERNEL_VMA_BOUNDARY) {
      file_put(af);
      return (int64_t)-EFAULT;
    }
  }
  if (addrlen) {
    uint64_t alptr = (int64_t)addrlen;
    if (alptr >= KERNEL_VMA_BOUNDARY ||
        alptr + sizeof(socklen_t) > KERNEL_VMA_BOUNDARY) {
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
      // Blocking: block — 持 socket_lock 挂 wq（模式2）
      wait_queue_t wait;
      wait.func = poll_wait_cb;
      wait.data = proc;
      wait.exclusive = 0;
      list_init(&wait.node);
      add_wait_queue(listen_sock->wq, &wait);
      proc->state = BLOCKED;
      proc->wait_event = WAIT_POLL;
      // Linux blocking accept has no built-in timeout: it waits indefinitely
      // for a connection, returning only on a signal (EINTR) or fd close.
      // The previous 30s wait_deadline forced -ETIMEDOUT on idle listeners,
      // making long-lived servers (udevd/shell) wrongly bail out. Wait forever,
      // but borrow an armed process alarm deadline (if any) as the wake
      // deadline so the timer queue can fire SIGALRM and interrupt via EINTR —
      // mirrors sys_epoll_wait / F_SETLKW (indefinite, signal-interruptible).
      uint64_t alarm_dl = 0;
      if (proc->proc && proc->proc->signal) {
        uint64_t sflags;
        spin_lock_irqsave(&proc->proc->signal->sig_lock, &sflags);
        alarm_dl = proc->proc->signal->alarm_deadline;
        spin_unlock_irqrestore(&proc->proc->signal->sig_lock, sflags);
      }
      if (alarm_dl != 0) {
        proc->wait_deadline = alarm_dl;
        int cpu = proc->assigned_cpu;
        uint64_t rflags;
        spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &rflags);
        sched_timer_queue_insert(cpu, proc);
        spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, rflags);
      } else {
        proc->wait_deadline = 0;
      }
      spin_unlock(&socket_lock);
      schedule();
      // EINTR check (signal is the only way out of an idle blocking accept)
      {
        uint64_t pend =
            __atomic_load_n(&proc->proc->sig_pending, __ATOMIC_ACQUIRE);
        uint64_t deliv = pend & ~proc->proc->sig_blocked;
        deliv |= (pend & ((SIGMASK(SIGKILL)) | (SIGMASK(SIGSTOP))));
        if (deliv) {
          proc->state = RUNNING;
          remove_wait_queue(listen_sock->wq, &wait);
          ret = -EINTR;
          goto out;
        }
      }
      proc->state = RUNNING;
      remove_wait_queue(listen_sock->wq, &wait);
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
  if (!addr_ptr || addr_ptr >= KERNEL_VMA_BOUNDARY ||
      addr_ptr + addrlen > KERNEL_VMA_BOUNDARY || addrlen <= 0) {
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

  // DGRAM connect: just fix the default send target. No child socket, no
  // backlog, no accept wake — Linux semantics for SOCK_DGRAM connect().
  struct unix_sock *client_sock = cf->sock;
  if (!client_sock) {
    spin_unlock(&socket_lock);
    file_put(cf);
    return (int64_t)-EBADF;
  }
  if (client_sock->type == SOCK_DGRAM) {
    struct unix_sock *target = NULL;
    int dret = unix_bind_lookup_dgram(sun_path, &target);
    if (dret) {
      spin_unlock(&socket_lock);
      file_put(cf);
      return (int64_t)dret;
    }
    // DGRAM connect: 只缓存目标 path，不持 peer_sock 指针。对端 dgram
    // socket 仅靠自身 fd 存活且无反向引用，缓存指针会在对端 close 后悬空
    // （close 本端时读到已释放内存 → UAF）。发送时按 path 动态查找
    // （unix_dgram_sendto 对 target 取临时 u_count 引用，wake-after-unlock
    // 安全）。Linux DGRAM connect 同样不持久绑定 peer 对象。
    __memcpy(client_sock->dgram_dst_path, sun_path,
             sizeof(client_sock->dgram_dst_path));
    client_sock->dgram_dst_path[sizeof(client_sock->dgram_dst_path) - 1] = '\0';
    client_sock->state = UNIX_CONNECTED;
    spin_unlock(&socket_lock);
    file_put(cf);
    return 0;
  }

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
  struct unix_sock *client_sock2 = cf->sock;
  if (!client_sock2) {
    spin_unlock(&socket_lock);
    unix_sock_free(child);
    file_put(cf);
    return (int64_t)-EBADF;
  }
  client_sock2->state = UNIX_CONNECTED;

  client_sock2->peer = listener_pid;
  client_sock2->peer_sock = child; // client's peer is the child (server-side)
  child->peer = proc->pid;
  child->peer_sock = client_sock2; // child's peer is the client

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

  // Wake listener's blocked accept (挂 listener->wq)
  spin_unlock(&socket_lock);

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
  int base_type = type & (SOCK_STREAM | SOCK_DGRAM | SOCK_SEQPACKET);
  if (base_type != SOCK_STREAM && base_type != SOCK_DGRAM)
    return (int64_t)-EPROTONOSUPPORT;
  if (protocol != 0)
    return (int64_t)-EPROTONOSUPPORT;

  // Validate user pointer
  uint64_t sv_ptr = (int64_t)sv;
  if (!sv_ptr || sv_ptr >= KERNEL_VMA_BOUNDARY ||
      sv_ptr + 2 * sizeof(int) > KERNEL_VMA_BOUNDARY)
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
  a->type = base_type;
  a->peer = proc->pid;
  a->peer_sock = b;
  b->state = UNIX_CONNECTED;
  b->type = base_type;
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
  a->type = base_type;
  a->peer = proc->pid;
  a->peer_sock = b;
  b->state = UNIX_CONNECTED;
  b->type = base_type;
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
  if (!msg_ptr || msg_ptr >= KERNEL_VMA_BOUNDARY ||
      msg_ptr + sizeof(struct msghdr) > KERNEL_VMA_BOUNDARY) {
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
  if (!iov_ptr || iov_ptr >= KERNEL_VMA_BOUNDARY ||
      iov_ptr + kmsg.msg_iovlen * sizeof(struct iovec) > KERNEL_VMA_BOUNDARY ||
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
      if (base >= KERNEL_VMA_BOUNDARY ||
          base + kiov[i].iov_len > KERNEL_VMA_BOUNDARY) {
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
    if (ctrl_ptr >= KERNEL_VMA_BOUNDARY ||
        ctrl_ptr + kmsg.msg_controllen > KERNEL_VMA_BOUNDARY) {
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
    // DGRAM sendmsg with a destination address: resolve the path and enqueue
    // directly (no connect/listen). Without msg_name a connected DGRAM falls
    // through to unix_sock_sendmsg, which uses sock->peer_sock.
    if (sock->type == SOCK_DGRAM && kmsg.msg_name && kmsg.msg_namelen > 0) {
      struct sockaddr_un dest;
      __memset(&dest, 0, sizeof(dest));
      size_t dlen =
          kmsg.msg_namelen > sizeof(dest) ? sizeof(dest) : kmsg.msg_namelen;
      if (copy_from_user(&dest, (const void __user *)kmsg.msg_name, dlen)) {
        kfree(kiov);
        if (kcontrol)
          kfree(kcontrol);
        file_put(sf);
        return (int64_t)-EFAULT;
      }
      dest.sun_path[UNIX_PATH_MAX - 1] = '\0';
      ret = unix_dgram_sendto(sock, &dest, kiov, kmsg.msg_iovlen, kcontrol,
                              kcontrollen, flags);
    } else {
      ret = unix_sock_sendmsg(sock, kiov, kmsg.msg_iovlen, kcontrol,
                              kcontrollen, flags);
    }
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
  if (!msg_ptr || msg_ptr >= KERNEL_VMA_BOUNDARY ||
      msg_ptr + sizeof(struct msghdr) > KERNEL_VMA_BOUNDARY) {
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
  if (!iov_ptr || iov_ptr >= KERNEL_VMA_BOUNDARY ||
      iov_ptr + kmsg.msg_iovlen * sizeof(struct iovec) > KERNEL_VMA_BOUNDARY ||
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
      if (base >= KERNEL_VMA_BOUNDARY ||
          base + kiov[i].iov_len > KERNEL_VMA_BOUNDARY) {
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
    if (ctrl_ptr >= KERNEL_VMA_BOUNDARY ||
        ctrl_ptr + kmsg.msg_controllen > KERNEL_VMA_BOUNDARY) {
      kfree(kiov);
      file_put(rf);
      return (int64_t)-EFAULT;
    }
    kcontrol = (void *)ctrl_ptr; // write directly to user space
    kcontrollen = kmsg.msg_controllen;
  }

  // recvmsg(2): msg_flags is an output field; the value passed in is ignored
  // and the kernel writes the result flags (e.g. MSG_TRUNC/MSG_CTRUNC) here.
  // Zero it before the recv so unix_sock_recvmsg can OR in status bits.
  kmsg.msg_flags = 0;

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
    if (sock->type == SOCK_DGRAM) {
      sockaddr_un sender_addr;
      size_t sender_len = 0;
      __memset(&sender_addr, 0, sizeof(sender_addr));
      ret = unix_dgram_recvmsg(sock, kiov, kmsg.msg_iovlen, kcontrol,
                               &kcontrollen, flags, &kmsg.msg_flags,
                               &sender_addr, &sender_len);
      // Write back sender address to msg_name/msg_namelen (netlink-style).
      if (ret >= 0 && kmsg.msg_name && sender_len > 0 &&
          kmsg.msg_namelen >= sender_len) {
        if (copy_to_user((void __user *)kmsg.msg_name, &sender_addr,
                         sender_len))
          ret = (int64_t)-EFAULT;
        else {
          socklen_t out_len = (socklen_t)sender_len;
          if (copy_to_user((void __user *)((char __user *)msg + 4), &out_len,
                           sizeof(socklen_t)))
            ret = (int64_t)-EFAULT;
        }
      }
    } else {
      ret = unix_sock_recvmsg(sock, kiov, kmsg.msg_iovlen, kcontrol,
                              &kcontrollen, flags, &kmsg.msg_flags);
    }
    // Update msg_controllen in user space (existing code)
    if (ret >= 0) {
      if (kmsg.msg_control && kmsg.msg_controllen > 0) {
        if (copy_to_user((void __user *)((char __user *)msg + 40), &kcontrollen,
                         sizeof(size_t)))
          ret = (int64_t)-EFAULT;
      }
      // Write back msg_flags (MSG_TRUNC/MSG_CTRUNC) so the caller can detect
      // truncation. Use offsetof rather than a hardcoded offset so this stays
      // correct if the msghdr layout ever changes. user/kernel share the same
      // <xos/socket.h> msghdr, so the offset is identical.
      if (ret >= 0) {
        size_t flags_off = offsetof(struct msghdr, msg_flags);
        if (copy_to_user((void __user *)((char __user *)msg + flags_off),
                         &kmsg.msg_flags, sizeof(int)))
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

  struct unix_sock *peer_s = sock->peer_sock;

  spin_unlock(&socket_lock);

  // 唤醒本端与对端阻塞 reader/writer（各自挂自己 wq）
  __wake_up(sock->wq, POLLHUP | POLLIN | POLLOUT);
  if (peer_s)
    __wake_up(peer_s->wq, POLLHUP | POLLIN | POLLOUT);

  file_put(shf);
  return 0;
}

// Per-fd wait callback for sys_poll: registered on each polled fd's wq so a
// fd becoming ready (e.g. timerfd expiry) wakes the blocked poll promptly
// instead of waiting out the full deadline. Mirrors eventfd/timerfd wake cbs.
static void poll_wait_cb(wait_queue_t *wq, unsigned long flags) {
  xtask *proc = (xtask *)wq->data;
  (void)flags;
  wake_wq_target(proc);
}

int64_t sys_poll(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                 int64_t unused2, int64_t unused3) {
  struct pollfd __user *fds = (struct pollfd __user *)arg1;
  nfds_t nfds = (nfds_t)arg2;
  int timeout_ms = (int)arg3;

  xtask *proc = current_task;

  // Validate user pointer
  uint64_t fds_ptr = (int64_t)fds;
  if (!fds_ptr || fds_ptr >= KERNEL_VMA_BOUNDARY ||
      fds_ptr + nfds * sizeof(struct pollfd) > KERNEL_VMA_BOUNDARY)
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
    // prepare_to_wait: 先标 BLOCKED 再 re-poll，使 fd 的 __wake_up
    // 若在重查后到达 命中已 BLOCKED 的 task（poll 是多 wq，pwq[]
    // 注册结构保留，只动 state 位置）。
    proc->state = BLOCKED;
    proc->wait_event = WAIT_POLL;
    proc->wait_timed_out = 0;
    int ready = 0;

    // Check each fd and register a wait entry on its wq.
    for (nfds_t i = 0; i < nfds; i++) {
      kfds[i].revents = 0;
      int pfd = kfds[i].fd;

      if (pfd < 0 || pfd >= MAX_FD) {
        // Invalid fd (out of range or closed): Linux returns POLLNVAL, not
        // POLLERR. POLLERR is reserved for "fd valid but underlying error"
        // (e.g. socket error); an unresolvable fd is POLLNVAL.
        kfds[i].revents = POLLNVAL;
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
        kfds[i].revents = POLLNVAL;
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
        for (nfds_t i = 0; i < nfds; i++)
          kfds[i].revents = 0;
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

    schedule();

    // EINTR check (before timeout check: signal priority over timeout)
    {
      uint64_t pend =
          __atomic_load_n(&proc->proc->sig_pending, __ATOMIC_ACQUIRE);
      uint64_t deliv = pend & ~proc->proc->sig_blocked;
      deliv |= (pend & ((SIGMASK(SIGKILL)) | (SIGMASK(SIGSTOP))));
      if (deliv) {
        ret = (int64_t)-EINTR;
        goto poll_out;
      }
    }

    if (proc->wait_timed_out && timeout_ms > 0) {
      for (nfds_t i = 0; i < nfds; i++)
        kfds[i].revents = 0;
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
  // prepare_to_wait: 循环顶部标过 BLOCKED，若 re-poll 期间某 fd 的 poll_wait_cb
  // 命中 wake_wq_target 把 run_node push 进了 run_queue（state=READY），goto
  // poll_out 不走 schedule() 会留下悬空 run_node（下次 block+wake 撞
  // run_queue_push 单租户 ASSERT， 或被 steal 偷到 state≠READY 撞
  // sched.c:382）。cancel 掉虚假唤醒：摘 run_node + state=RUNNING。所有 goto
  // poll_out 路径统一在此处理。
  sched_cancel_spurious_wake(proc);
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

int64_t unix_sock_write(struct unix_sock *sock, const void *buf, size_t len,
                        int flags) {
  struct iovec iov;
  iov.iov_base = (void *)buf;
  iov.iov_len = len;
  return unix_sock_sendmsg(sock, &iov, 1, NULL, 0, flags);
}

int64_t unix_sock_read(struct unix_sock *sock, void *buf, size_t len,
                       int flags) {
  struct iovec iov;
  iov.iov_base = buf;
  iov.iov_len = len;
  int dummy_flags = 0;
  return unix_sock_recvmsg(sock, &iov, 1, NULL, NULL, flags, &dummy_flags);
}

// A6: recvfrom(fd, buf, len, flags, addr, addrlen) — thin wrapper over recvmsg
int64_t sys_recvfrom(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4,
                     int64_t arg5, int64_t arg6) {
  int fd = (int)arg1;
  void __user *buf = (void __user *__force)arg2;
  size_t len = (size_t)arg3;
  int flags = (int)arg4;
  struct sockaddr __user *addr = (struct sockaddr __user * __force) arg5;
  socklen_t __user *addrlen = (socklen_t __user * __force) arg6;

  xtask *proc = current_task;
  if (fd < 0 || fd >= MAX_FD)
    return -EBADF;

  rcu_read_lock();
  struct file *f = fd_lookup(proc->proc->files, fd);
  if (!f) {
    rcu_read_unlock();
    return -EBADF;
  }
  file_get(f);
  rcu_read_unlock();

  int64_t ret;
  if (f->type != FD_SOCKET) {
    ret = -ENOTSOCK;
    goto out;
  }
  struct unix_sock *sock = f->sock;
  if (!sock) {
    ret = -EBADF;
    goto out;
  }

  if (!buf) {
    ret = -EFAULT;
    goto out;
  }
  uint64_t ptr_start = (__force uint64_t)buf;
  uint64_t ptr_end = ptr_start + len;
  if (ptr_end < ptr_start || ptr_start >= KERNEL_VMA_BOUNDARY ||
      ptr_end > KERNEL_VMA_BOUNDARY) {
    ret = -EFAULT;
    goto out;
  }

  // DGRAM: the source address comes from the datagram's sender (which may
  // differ per message), not from a fixed peer_sock. Route through the DGRAM
  // recvmsg path so the sender address is captured.
  if (sock->type == SOCK_DGRAM) {
    struct iovec iov = {.iov_base = (void __force *)buf, .iov_len = len};
    struct sockaddr_un su;
    size_t su_len = 0;
    __memset(&su, 0, sizeof(su));
    ret = unix_dgram_recvmsg(sock, &iov, 1, NULL, NULL, flags, NULL, &su,
                             &su_len);
    if (addr && ret >= 0 && su_len > 0) {
      socklen_t actual_len = (socklen_t)su_len;
      if (copy_to_user(addr, &su, actual_len))
        ret = -EFAULT;
      if (addrlen) {
        uint64_t al = (__force uint64_t)addrlen;
        if (al < KERNEL_VMA_BOUNDARY)
          *(__force socklen_t *)addrlen = actual_len;
      }
    }
    goto out;
  }

  ret = unix_sock_read(sock, (void __force *)buf, len, flags);

  // Fill addr if requested (peer address for connected socket)
  if (addr && ret > 0 && sock->peer_sock) {
    struct sockaddr_un su;
    __memset(&su, 0, sizeof(su));
    su.sun_family = AF_UNIX;
    if (sock->peer_sock->sun_path[0])
      __memcpy(su.sun_path, sock->peer_sock->sun_path, UNIX_PATH_MAX);
    socklen_t actual_len = sizeof(sa_family_t);
    if (su.sun_path[0])
      actual_len = sizeof(struct sockaddr_un);
    if (copy_to_user(addr, &su, actual_len))
      ret = -EFAULT;
    if (addrlen) {
      uint64_t al = (__force uint64_t)addrlen;
      if (al < KERNEL_VMA_BOUNDARY)
        *(__force socklen_t *)addrlen = actual_len;
    }
  }

out:
  file_put(f);
  return ret;
}

// A7: sendto(fd, buf, len, flags, addr, addrlen) — thin wrapper over sendmsg
int64_t sys_sendto(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4,
                   int64_t arg5, int64_t arg6) {
  int fd = (int)arg1;
  const void __user *buf = (const void __user *__force)arg2;
  size_t len = (size_t)arg3;
  int flags = (int)arg4;
  const struct sockaddr __user *addr =
      (const struct sockaddr __user *__force)arg5;
  socklen_t addrlen = (socklen_t)arg6;

  xtask *proc = current_task;
  if (fd < 0 || fd >= MAX_FD)
    return -EBADF;

  rcu_read_lock();
  struct file *f = fd_lookup(proc->proc->files, fd);
  if (!f) {
    rcu_read_unlock();
    return -EBADF;
  }
  file_get(f);
  rcu_read_unlock();

  int64_t ret;
  if (f->type != FD_SOCKET) {
    ret = -ENOTSOCK;
    goto out;
  }
  struct unix_sock *sock = f->sock;
  if (!sock) {
    ret = -EBADF;
    goto out;
  }
  if (!buf) {
    ret = -EFAULT;
    goto out;
  }
  uint64_t ptr_start = (__force uint64_t)buf;
  uint64_t ptr_end = ptr_start + len;
  if (ptr_end < ptr_start || ptr_start >= KERNEL_VMA_BOUNDARY ||
      ptr_end > KERNEL_VMA_BOUNDARY) {
    ret = -EFAULT;
    goto out;
  }

  // For AF_UNIX, sendto to a connected socket ignores addr (uses peer).
  // A DGRAM socket with an explicit dest addr resolves the path and enqueues
  // directly into the target's recv queue (no connect/listen needed).
  if (addr && addrlen > 0) {
    if (addrlen < sizeof(sa_family_t)) {
      ret = -EINVAL;
      goto out;
    }
    if (sock->type == SOCK_DGRAM) {
      struct sockaddr_un dest;
      __memset(&dest, 0, sizeof(dest));
      size_t cpy = addrlen > sizeof(dest) ? sizeof(dest) : addrlen;
      if (copy_from_user(&dest, addr, cpy)) {
        ret = -EFAULT;
        goto out;
      }
      dest.sun_path[UNIX_PATH_MAX - 1] = '\0';
      struct iovec iov = {.iov_base = (void __force *)buf, .iov_len = len};
      file_put(f);
      return unix_dgram_sendto(sock, &dest, &iov, 1, NULL, 0, flags);
    }
    // STREAM + addr: ignore addr, fall through to peer_sock send (Linux:
    // connected stream sendto ignores the supplied address).
    if (!sock->peer_sock) {
      ret = -ENOTCONN;
      goto out;
    }
  }

  ret = unix_sock_write(sock, (const void __force *)buf, len, flags);

out:
  file_put(f);
  return ret;
}

// F1: getsockname(fd, addr, addrlen) — return bound address of socket
int64_t sys_getsockname(int64_t arg1, int64_t arg2, int64_t arg3,
                        int64_t unused1, int64_t unused2, int64_t unused3) {
  int fd = (int)arg1;
  struct sockaddr __user *addr = (struct sockaddr __user * __force) arg2;
  socklen_t __user *addrlen_ptr = (socklen_t __user * __force) arg3;

  if (fd < 0 || fd >= MAX_FD)
    return -EBADF;

  xtask *proc = current_task;
  rcu_read_lock();
  struct file *f = fd_lookup(proc->proc->files, fd);
  if (!f) {
    rcu_read_unlock();
    return -EBADF;
  }
  file_get(f);
  rcu_read_unlock();

  int64_t ret;
  if (f->type != FD_SOCKET) {
    ret = -ENOTSOCK;
    goto out;
  }
  struct unix_sock *sock = f->sock;
  if (!sock) {
    ret = -EBADF;
    goto out;
  }

  struct sockaddr_un su;
  __memset(&su, 0, sizeof(su));
  su.sun_family = AF_UNIX;
  if (sock->sun_path[0])
    __memcpy(su.sun_path, sock->sun_path, UNIX_PATH_MAX);

  socklen_t actual_len = sizeof(sa_family_t);
  if (su.sun_path[0])
    actual_len = sizeof(struct sockaddr_un);

  if (addr) {
    if (copy_to_user(addr, &su, actual_len)) {
      ret = -EFAULT;
      goto out;
    }
  }
  if (addrlen_ptr) {
    uint64_t al = (__force uint64_t)addrlen_ptr;
    if (al >= KERNEL_VMA_BOUNDARY) {
      ret = -EFAULT;
      goto out;
    }
    *(__force socklen_t *)addrlen_ptr = actual_len;
  }
  ret = 0;

out:
  file_put(f);
  return ret;
}

// F2: getpeername(fd, addr, addrlen) — return peer address of connected socket
int64_t sys_getpeername(int64_t arg1, int64_t arg2, int64_t arg3,
                        int64_t unused1, int64_t unused2, int64_t unused3) {
  int fd = (int)arg1;
  struct sockaddr __user *addr = (struct sockaddr __user * __force) arg2;
  socklen_t __user *addrlen_ptr = (socklen_t __user * __force) arg3;

  if (fd < 0 || fd >= MAX_FD)
    return -EBADF;

  xtask *proc = current_task;
  rcu_read_lock();
  struct file *f = fd_lookup(proc->proc->files, fd);
  if (!f) {
    rcu_read_unlock();
    return -EBADF;
  }
  file_get(f);
  rcu_read_unlock();

  int64_t ret;
  if (f->type != FD_SOCKET) {
    ret = -ENOTSOCK;
    goto out;
  }
  struct unix_sock *sock = f->sock;
  if (!sock) {
    ret = -EBADF;
    goto out;
  }
  if (!sock->peer_sock) {
    ret = -ENOTCONN;
    goto out;
  }

  struct sockaddr_un su;
  __memset(&su, 0, sizeof(su));
  su.sun_family = AF_UNIX;
  if (sock->peer_sock->sun_path[0])
    __memcpy(su.sun_path, sock->peer_sock->sun_path, UNIX_PATH_MAX);

  socklen_t actual_len = sizeof(sa_family_t);
  if (su.sun_path[0])
    actual_len = sizeof(struct sockaddr_un);

  if (addr) {
    if (copy_to_user(addr, &su, actual_len)) {
      ret = -EFAULT;
      goto out;
    }
  }
  if (addrlen_ptr) {
    uint64_t al = (__force uint64_t)addrlen_ptr;
    if (al >= KERNEL_VMA_BOUNDARY) {
      ret = -EFAULT;
      goto out;
    }
    *(__force socklen_t *)addrlen_ptr = actual_len;
  }
  ret = 0;

out:
  file_put(f);
  return ret;
}

// F3: getsockopt(fd, level, optname, optval, optlen) — return defaults for
// common options
int64_t sys_getsockopt(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4,
                       int64_t arg5, int64_t unused) {
  int fd = (int)arg1;
  int level = (int)arg2;
  int optname = (int)arg3;
  void __user *optval = (void __user *__force)arg4;
  socklen_t __user *optlen_ptr = (socklen_t __user * __force) arg5;

  if (fd < 0 || fd >= MAX_FD)
    return -EBADF;

  xtask *proc = current_task;
  rcu_read_lock();
  struct file *f = fd_lookup(proc->proc->files, fd);
  if (!f) {
    rcu_read_unlock();
    return -EBADF;
  }
  file_get(f);
  rcu_read_unlock();

  int64_t ret;
  if (f->type != FD_SOCKET) {
    ret = -ENOTSOCK;
    goto out;
  }

  if (level != SOL_SOCKET) {
    ret = -ENOPROTOOPT;
    goto out;
  }

  switch (optname) {
  case SO_TYPE: {
    int val = SOCK_STREAM;
    if (optval && optlen_ptr) {
      socklen_t len = *(__force socklen_t *)optlen_ptr;
      if (len < sizeof(int)) {
        ret = -EINVAL;
        goto out;
      }
      if (copy_to_user(optval, &val, sizeof(int))) {
        ret = -EFAULT;
        goto out;
      }
      *(__force socklen_t *)optlen_ptr = sizeof(int);
    }
    ret = 0;
    goto out;
  }
  case SO_ERROR: {
    int val = 0;
    if (optval && optlen_ptr) {
      socklen_t len = *(__force socklen_t *)optlen_ptr;
      if (len < sizeof(int)) {
        ret = -EINVAL;
        goto out;
      }
      if (copy_to_user(optval, &val, sizeof(int))) {
        ret = -EFAULT;
        goto out;
      }
      *(__force socklen_t *)optlen_ptr = sizeof(int);
    }
    ret = 0;
    goto out;
  }
  case SO_SNDBUF: {
    int val = 8192;
    if (optval && optlen_ptr) {
      socklen_t len = *(__force socklen_t *)optlen_ptr;
      if (len < sizeof(int)) {
        ret = -EINVAL;
        goto out;
      }
      if (copy_to_user(optval, &val, sizeof(int))) {
        ret = -EFAULT;
        goto out;
      }
      *(__force socklen_t *)optlen_ptr = sizeof(int);
    }
    ret = 0;
    goto out;
  }
  case SO_RCVBUF: {
    int val = 8192;
    if (optval && optlen_ptr) {
      socklen_t len = *(__force socklen_t *)optlen_ptr;
      if (len < sizeof(int)) {
        ret = -EINVAL;
        goto out;
      }
      if (copy_to_user(optval, &val, sizeof(int))) {
        ret = -EFAULT;
        goto out;
      }
      *(__force socklen_t *)optlen_ptr = sizeof(int);
    }
    ret = 0;
    goto out;
  }
  case SO_PEERCRED: // not implemented
  default:
    ret = -ENOPROTOOPT;
    goto out;
  }

out:
  file_put(f);
  return ret;
}

// F4: setsockopt(fd, level, optname, optval, optlen) — silent success for most
// options
int64_t sys_setsockopt(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4,
                       int64_t arg5, int64_t unused) {
  int fd = (int)arg1;
  (void)arg2; // level
  (void)arg3; // optname
  (void)arg4; // optval
  (void)arg5; // optlen

  if (fd < 0 || fd >= MAX_FD)
    return -EBADF;

  xtask *proc = current_task;
  rcu_read_lock();
  struct file *f = fd_lookup(proc->proc->files, fd);
  if (!f) {
    rcu_read_unlock();
    return -EBADF;
  }
  file_get(f);
  rcu_read_unlock();

  int64_t ret;
  if (f->type != FD_SOCKET) {
    ret = -ENOTSOCK;
    goto out;
  }

  // All socket options: silently return 0 (success but no-op)
  ret = 0;

out:
  file_put(f);
  return ret;
}

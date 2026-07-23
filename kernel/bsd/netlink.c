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
#include "kernel/bsd/netlink.h"
#include "kernel/bsd/proc.h"
#include "kernel/bsd/socket.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/xtask.h"
#include <xos/errno.h>
#include <xos/netlink.h>
#include <xos/signal.h>
#include <xos/socket.h>

// copy_from_user/copy_to_user have no dedicated header; forward-declare.
size_t copy_from_user(void *dst, const void *src, size_t size);
size_t copy_to_user(void *dst, const void *src, size_t size);

// netlink recv 阻塞点的 wq 回调：__wake_up → 唤醒挂在 sock->wq 的 reader。
// 不查 wait_event（队列身份制：在 wq 上即唤醒），与 socket/pty 一致。
static void nl_recv_wake_cb(wait_queue_t *wq, unsigned long flags) {
  xtask *target = (xtask *)wq->data;
  (void)flags;
  wake_wq_target(target);
}

// ===================== Global group lock =====================
spinlock nl_group_lock = SPINLOCK_INIT;

// ===================== Group registry =====================
static nl_group_member *nl_groups[NL_MAX_GROUPS];
static bool nl_initialized = false;

// ===================== sk_buff helpers for netlink =====================
// netlink_sock uses the same sk_buff layout as unix_sock, but
// enqueue/dequeue take netlink_sock* instead. The sk_buff->next field
// chains the recv_queue; data[] holds the message payload.

static void nl_skb_enqueue(netlink_sock *sock, struct sk_buff *skb) {
  skb->next = NULL;
  if (sock->recv_queue_tail) {
    sock->recv_queue_tail->next = skb;
  } else {
    sock->recv_queue_head = skb;
  }
  sock->recv_queue_tail = skb;
  sock->recv_queue_len++;
}

static struct sk_buff *nl_skb_dequeue(netlink_sock *sock) {
  if (!sock->recv_queue_head)
    return NULL;
  struct sk_buff *skb = sock->recv_queue_head;
  sock->recv_queue_head = skb->next;
  if (!sock->recv_queue_head)
    sock->recv_queue_tail = NULL;
  skb->next = NULL;
  sock->recv_queue_len--;
  return skb;
}

// ===================== netlink_sock lifecycle =====================

netlink_sock *netlink_sock_alloc(int protocol) {
  netlink_sock *sock = (netlink_sock *)kmalloc(sizeof(netlink_sock));
  if (!sock)
    return NULL;
  sock->groups = 0;
  sock->portid = 0;
  sock->protocol = protocol;
  sock->recv_queue_head = NULL;
  sock->recv_queue_tail = NULL;
  sock->recv_queue_len = 0;
  /* eager 分配 wq：netlink recv 阻塞点转 add_wait_queue 后 wq 必须非
   * NULL（§5.3）。 */
  sock->wq = (wait_queue_head *)kmalloc(sizeof(wait_queue_head));
  if (!sock->wq) {
    kfree(sock);
    return NULL;
  }
  init_wait_queue_head(sock->wq);
  refcount_set(&sock->n_count, 1);
  sock->owner_pid = current_task->pid;
  return sock;
}

void netlink_sock_free(netlink_sock *sock) {
  if (!sock)
    return;
  // Free remaining skbs
  while (sock->recv_queue_head) {
    struct sk_buff *skb = nl_skb_dequeue(sock);
    skb_free(skb);
  }
  if (sock->wq)
    kfree(sock->wq);
  kfree(sock);
}

void netlink_sock_release(netlink_sock *sock) {
  if (!sock)
    return;
  if (refcount_dec_and_test(&sock->n_count)) {
    nl_group_cleanup(sock);
    netlink_sock_free(sock);
  }
}

void netlink_sock_close(netlink_sock *sock) {
  if (!sock)
    return;

  spin_lock(&nl_group_lock);

  // Free all skbs in recv queue
  while (sock->recv_queue_head) {
    struct sk_buff *skb = nl_skb_dequeue(sock);
    skb_free(skb);
  }

  // Wake blocked reader
  spin_unlock(&nl_group_lock);

  // 唤醒阻塞 reader（挂 sock->wq）与 epoll 等待者
  __wake_up(sock->wq, POLLHUP | POLLIN);

  netlink_sock_release(sock);
}

// ===================== Wake helper =====================

// ===================== Bind =====================

int64_t netlink_sock_bind(netlink_sock *sock, const sockaddr_nl *addr) {
  if (addr->nl_family != AF_NETLINK)
    return -EAFNOSUPPORT;

  spin_lock(&nl_group_lock);

  // Assign portid: 0 means auto-assign = owner PID
  uint32_t requested_pid = addr->nl_pid;
  if (requested_pid == 0) {
    sock->portid = (uint32_t)sock->owner_pid;
  } else {
    sock->portid = requested_pid;
  }

  // Subscribe groups from nl_groups bitmask
  uint32_t groups = addr->nl_groups;
  sock->groups = groups;
  for (int bit = 0; bit < 32; bit++) {
    if (groups & (1u << bit)) {
      nl_group_subscribe(sock, (uint32_t)bit);
    }
  }

  spin_unlock(&nl_group_lock);
  return 0;
}

// ===================== Sendmsg =====================

int64_t netlink_sock_sendmsg(netlink_sock *sock, const struct iovec *iov,
                             size_t iovlen, int flags) {
  if (!sock->groups)
    return -ENOTCONN;

  // Calculate total data length
  uint32_t total = 0;
  for (size_t i = 0; i < iovlen; i++)
    total += iov[i].iov_len;
  if (total > MAX_SOCKET_DATA)
    return -EMSGSIZE;

  // Broadcast to all subscribed groups
  // Each iov segment is broadcast independently to keep message atomicity
  // (netlink datagram = one skb per sendmsg call)
  // We coalesce all iov into one contiguous buffer first.
  uint8_t *buf = (uint8_t *)kmalloc(total);
  if (!buf)
    return -ENOMEM;
  uint32_t offset = 0;
  for (size_t i = 0; i < iovlen; i++) {
    if (iov[i].iov_base && iov[i].iov_len > 0) {
      if (copy_from_user(buf + offset, (const void __user *)iov[i].iov_base,
                         iov[i].iov_len)) {
        kfree(buf);
        return -EFAULT;
      }
      offset += iov[i].iov_len;
    }
  }

  // Broadcast to each subscribed group (exclude self)
  for (int bit = 0; bit < 32; bit++) {
    if (sock->groups & (1u << bit)) {
      nl_group_broadcast((uint32_t)bit, buf, total, sock->owner_pid);
    }
  }

  kfree(buf);
  return (int64_t)total;
}

// ===================== Recvmsg =====================

int64_t netlink_sock_recvmsg(netlink_sock *sock, const struct iovec *iov,
                             size_t iovlen, sockaddr_nl *src_addr,
                             size_t *src_len, int flags) {
  bool nonblock = (flags & MSG_DONTWAIT) != 0;
  // Also check O_NONBLOCK on the file
  // (handled by caller: sys_recvmsg sets MSG_DONTWAIT if fd has O_NONBLOCK)

  while (1) {
    spin_lock(&nl_group_lock);

    struct sk_buff *skb = sock->recv_queue_head;
    if (!skb) {
      if (nonblock) {
        spin_unlock(&nl_group_lock);
        return -EAGAIN;
      }

      // Block reader: 持 nl_group_lock 挂 wq（模式2，条件检查与挂 wq
      // 同一临界区防丢唤醒）。
      xtask *proc = current_task;
      wait_queue_t wait;
      wait.func = nl_recv_wake_cb;
      wait.data = proc;
      wait.exclusive = 0;
      list_init(&wait.node);
      add_wait_queue(sock->wq, &wait);
      proc->state = BLOCKED;
      proc->wait_event = WAIT_POLL;
      proc->wait_deadline = sched_clock() + 30000000000ULL; // 30s timeout
      spin_unlock(&nl_group_lock);

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
          return (int64_t)-EINTR;
        }
      }

      proc->state = RUNNING;
      remove_wait_queue(sock->wq, &wait);
      continue; // 醒后回顶重新 lock → 重查 recv_queue
    }

    // We have data. Copy to user iov.
    uint32_t avail = skb->len - skb->consumed;
    uint32_t to_read = 0;
    for (size_t i = 0; i < iovlen && to_read < avail; i++) {
      uint32_t copy = (uint32_t)iov[i].iov_len;
      if (copy > avail - to_read)
        copy = avail - to_read;
      to_read += copy;
    }

    uint32_t data_offset = skb->consumed;
    uint32_t remaining = to_read;
    for (size_t i = 0; i < iovlen && remaining > 0; i++) {
      if (iov[i].iov_base && iov[i].iov_len > 0) {
        uint32_t copy = (uint32_t)iov[i].iov_len;
        if (copy > remaining)
          copy = remaining;
        if (copy_to_user((void __user *)iov[i].iov_base,
                         skb->data + data_offset, copy)) {
          spin_unlock(&nl_group_lock);
          return -EFAULT;
        }
        data_offset += copy;
        remaining -= copy;
      }
    }

    skb->consumed += to_read;
    bool skb_consumed = (skb->consumed >= skb->len);

    // Extract src_addr BEFORE freeing skb (must read skb->data while alive)
    if (src_addr && src_len && *src_len >= sizeof(sockaddr_nl)) {
      if (avail >= sizeof(nlmsghdr)) {
        nlmsghdr hdr;
        __memcpy(&hdr, skb->data + skb->consumed, sizeof(nlmsghdr));
        src_addr->nl_family = AF_NETLINK;
        src_addr->nl_pad = 0;
        src_addr->nl_pid = hdr.nlmsg_pid;
        src_addr->nl_groups = 0;
      } else {
        __memset(src_addr, 0, sizeof(sockaddr_nl));
        src_addr->nl_family = AF_NETLINK;
      }
      *src_len = sizeof(sockaddr_nl);
    }

    if (skb_consumed) {
      nl_skb_dequeue(sock);
      skb_free(skb);
    }

    spin_unlock(&nl_group_lock);
    return (int64_t)to_read;
  }
}

// ===================== Group operations =====================

int nl_group_subscribe(netlink_sock *sock, uint32_t group_bit) {
  if (group_bit >= NL_MAX_GROUPS)
    return -EINVAL;

  // nl_group_lock must already be held by caller
  nl_group_member *m = (nl_group_member *)kmalloc(sizeof(nl_group_member));
  if (!m)
    return -ENOMEM;
  m->sock = sock;
  m->next = nl_groups[group_bit];
  nl_groups[group_bit] = m;
  return 0;
}

int nl_group_leave(netlink_sock *sock, uint32_t group_bit) {
  if (group_bit >= NL_MAX_GROUPS)
    return -EINVAL;

  // nl_group_lock must already be held by caller
  nl_group_member **pp = &nl_groups[group_bit];
  while (*pp) {
    nl_group_member *m = *pp;
    if (m->sock == sock) {
      *pp = m->next;
      kfree(m);
      return 0;
    }
    pp = &m->next;
  }
  return -ENOENT;
}

void nl_group_cleanup(netlink_sock *sock) {
  spin_lock(&nl_group_lock);
  for (int bit = 0; bit < NL_MAX_GROUPS; bit++) {
    nl_group_member **pp = &nl_groups[bit];
    while (*pp) {
      nl_group_member *m = *pp;
      if (m->sock == sock) {
        *pp = m->next;
        kfree(m);
        // Don't break — same sock may appear in multiple groups
        continue;
      }
      pp = &m->next;
    }
  }
  spin_unlock(&nl_group_lock);
}

// ===================== Broadcast =====================

void nl_group_broadcast(uint32_t group_bit, const void *data, size_t len,
                        pid_t exclude_pid) {
  if (group_bit >= NL_MAX_GROUPS || !nl_initialized)
    return;

  spin_lock(&nl_group_lock);

  for (nl_group_member *m = nl_groups[group_bit]; m; m = m->next) {
    netlink_sock *sock = m->sock;
    if (sock->owner_pid == exclude_pid)
      continue;

    // Queue overflow: drop oldest
    if (sock->recv_queue_len >= NL_RECV_QUEUE_LIMIT) {
      struct sk_buff *old = nl_skb_dequeue(sock);
      if (old)
        skb_free(old);
    }

    struct sk_buff *skb = skb_alloc((uint32_t)len);
    if (!skb)
      continue; // skip this member on alloc failure
    __memcpy(skb->data, data, len);
    nl_skb_enqueue(sock, skb);

    // 唤醒阻塞 reader（挂 sock->wq）与 epoll 等待者，统一走 __wake_up
    __wake_up(sock->wq, POLLIN);
  }

  spin_unlock(&nl_group_lock);
}

// ===================== Initialization =====================

void nl_init(void) {
  spin_lock(&nl_group_lock);
  for (int i = 0; i < NL_MAX_GROUPS; i++)
    nl_groups[i] = NULL;
  nl_initialized = true;
  spin_unlock(&nl_group_lock);
  printk(LOG_INFO, "nl_init: done\n");
}

bool nl_is_initialized(void) { return nl_initialized; }

// Append bytes from s to payload buffer at pos (no NUL terminator).
// Returns new pos, clamped to buffer size.
static int nl_put(char *buf, int pos, int cap, const char *s) {
  while (*s && pos < cap)
    buf[pos++] = *s++;
  return pos;
}

// Append a NUL terminator (field separator). Returns new pos.
static int nl_put_sep(char *buf, int pos, int cap) {
  if (pos < cap)
    buf[pos++] = '\0';
  return pos;
}

void nl_uevent_broadcast(const char *action, const char *devpath,
                         const char *subsystem) {
  if (!nl_initialized)
    return;

  // Build uevent payload:
  //   "action@devpath\0ACTION=action\0DEVPATH=devpath\0SUBSYSTEM=subsystem\0"
  char payload[256];
  int pos = 0;
  int cap = (int)sizeof(payload);

  // "action@devpath\0"
  pos = nl_put(payload, pos, cap, action);
  pos = nl_put(payload, pos, cap, "@");
  pos = nl_put(payload, pos, cap, devpath);
  pos = nl_put_sep(payload, pos, cap);

  // "ACTION=action\0"
  pos = nl_put(payload, pos, cap, "ACTION=");
  pos = nl_put(payload, pos, cap, action);
  pos = nl_put_sep(payload, pos, cap);

  // "DEVPATH=devpath\0"
  pos = nl_put(payload, pos, cap, "DEVPATH=");
  pos = nl_put(payload, pos, cap, devpath);
  pos = nl_put_sep(payload, pos, cap);

  // "SUBSYSTEM=subsystem\0"
  pos = nl_put(payload, pos, cap, "SUBSYSTEM=");
  pos = nl_put(payload, pos, cap, subsystem);
  pos = nl_put_sep(payload, pos, cap);

  int payload_len = pos;

  // Wrap in nlmsghdr
  int total_len = NLMSG_LENGTH(payload_len);
  char msg[256 + NLMSG_HDRLEN];
  struct nlmsghdr *nh = (struct nlmsghdr *)msg;
  nh->nlmsg_len = (uint32_t)total_len;
  if (action[0] == 'a' && action[1] == 'd' && action[2] == 'd')
    nh->nlmsg_type = NLMSG_UEVENT_ADD;
  else if (action[0] == 'r' && action[1] == 'e')
    nh->nlmsg_type = NLMSG_UEVENT_REMOVE;
  else
    nh->nlmsg_type = NLMSG_UEVENT_CHANGE;
  nh->nlmsg_flags = 0;
  nh->nlmsg_seq = 0;
  nh->nlmsg_pid = 0; // kernel sender = portid 0
  __memcpy(NLMSG_DATA(nh), payload, payload_len);

  // Broadcast to NETLINK_KOBJECT_UEVENT group (bit 0 = group 1)
  nl_group_broadcast(0, msg, (size_t)total_len, -1);
}

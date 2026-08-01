/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_NETLINK_H
#define KERNEL_NETLINK_H

#include <stdbool.h>

#include "kernel/xcore/atomic.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/wait_queue.h"
#include <stdint.h>
#include <xos/netlink.h>
#include <xos/types.h> // pid_t

struct iovec;

// ===================== netlink_sock =====================
#define NL_RECV_QUEUE_LIMIT 256
#define NL_MAX_GROUPS 32

typedef struct netlink_sock {
  uint32_t groups; // subscribed group bitmask
  uint32_t portid; // bound port ID (default = PID)
  int protocol;    // e.g. NETLINK_KOBJECT_UEVENT

  // Receive queue (reuses sk_buff)
  struct sk_buff *recv_queue_head;
  struct sk_buff *recv_queue_tail;
  int recv_queue_len;

  // Wait queue (epoll integration); lazily allocated, epoll waiters park here
  wait_queue_head *wq;

  refcount_t n_count; // fd ref count (dup2 sharing)
  pid_t owner_pid;    // PID of the process that created this socket
} netlink_sock;

// ===================== nl_group registry =====================
typedef struct nl_group_member {
  struct netlink_sock *sock;
  struct nl_group_member *next;
} nl_group_member;

// ===================== Socket lifecycle =====================
netlink_sock *netlink_sock_alloc(int protocol);
void netlink_sock_free(netlink_sock *sock);
void netlink_sock_release(netlink_sock *sock);
void netlink_sock_close(netlink_sock *sock);

// ===================== Syscall paths =====================
int64_t netlink_sock_bind(netlink_sock *sock, const sockaddr_nl *addr);
int64_t netlink_sock_sendmsg(netlink_sock *sock, const struct iovec *iov,
                             size_t iovlen, int flags);
int64_t netlink_sock_recvmsg(netlink_sock *sock, const struct iovec *iov,
                             size_t iovlen, sockaddr_nl *src_addr,
                             size_t *src_len, int flags);

// ===================== Group operations =====================
int nl_group_subscribe(netlink_sock *sock, uint32_t group_bit);
int nl_group_leave(netlink_sock *sock, uint32_t group_bit);
void nl_group_cleanup(netlink_sock *sock);

// ===================== Broadcast primitive =====================
void nl_group_broadcast(uint32_t group_bit, const void *data, size_t len,
                        pid_t exclude_pid);

// ===================== Initialization =====================
void nl_init(void);
bool nl_is_initialized(void);

// ===================== Global lock =====================
extern spinlock nl_group_lock;

// Convenience: broadcast uevent with nlmsghdr framing
void nl_uevent_broadcast(const char *action, const char *devpath,
                         const char *subsystem);

#endif // KERNEL_NETLINK_H

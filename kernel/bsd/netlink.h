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
  uint32_t groups; // 已订阅的 group bitmask
  uint32_t portid; // 绑定的端口 ID（默认 = PID）
  int protocol;    // NETLINK_KOBJECT_UEVENT 等

  // Receive queue（复用 sk_buff）
  struct sk_buff *recv_queue_head;
  struct sk_buff *recv_queue_tail;
  int recv_queue_len;
  pid_t blocked_reader; // recvmsg 阻塞的 PID（-1 = none）

  // Wait queue（epoll 集成）
  wait_queue_head *wq; // 惰性分配，epoll 等待者挂此

  refcount_t n_count; // fd ref count（dup2 sharing）
  pid_t owner_pid;    // 创建此 socket 的进程 PID
} netlink_sock;

// ===================== nl_group 注册表 =====================
typedef struct nl_group_member {
  struct netlink_sock *sock;
  struct nl_group_member *next;
} nl_group_member;

// ===================== Socket lifecycle =====================
netlink_sock *netlink_sock_alloc(int protocol);
void netlink_sock_free(netlink_sock *sock);
void netlink_sock_release(netlink_sock *sock);
void netlink_sock_close(netlink_sock *sock);

// ===================== Syscall 路径 =====================
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

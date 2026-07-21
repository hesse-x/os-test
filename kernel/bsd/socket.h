/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_SOCKET_H
#define KERNEL_SOCKET_H

#include "kernel/xcore/atomic.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/wait_queue.h"
#include <stdint.h>

// ===================== sk_buff (socket buffer) =====================
// One per sendmsg call. Flexible array member for data.
#define MAX_SOCKET_DATA 65536 // 64KB soft limit (same as sys_msg)
#define MAX_SCM_FDS 8         // max fd count per SCM_RIGHTS

struct iovec;

typedef struct sk_buff {
  struct sk_buff *next; // linked list next
  uint32_t len;         // data length
  uint32_t consumed;    // bytes already read (SOCK_STREAM partial read)
  int num_fds;          // number of SCM_RIGHTS files carried
  struct file *files[MAX_SCM_FDS]; // SCM_RIGHTS files (ref-held at sendmsg)
  uint8_t data[];                  // flexible array member
} sk_buff;

// allocate: skb = kmalloc(sizeof(sk_buff) + data_len)
// free: kfree(skb) — skb_free() releases any ref-held files first.

// ===================== unix_sock state =====================
#define UNIX_MAX_BACKLOG 8

typedef enum unix_sock_state {
  UNIX_FREE = 0,
  UNIX_LISTEN,
  UNIX_CONNECTED,
  UNIX_CLOSED,
} unix_sock_state;

// ===================== unix_sock (per-socket kernel structure)
// =====================
typedef struct unix_sock {
  int state;  // UNIX_* state
  pid_t peer; // peer PID (CONNECTED)
  struct unix_sock
      *peer_sock;     // direct pointer to peer socket (socketpair/connect)
  refcount_t u_count; // fd ref count (dup2 sharing)

  // Receive queue (skb linked list)
  struct sk_buff *recv_queue_head;
  struct sk_buff *recv_queue_tail;
  int recv_queue_len;
  // 阻塞等待者改挂 sock->wq，不再记录 pid（队列身份制，§5.1）

  // Listen backlog (un-accepted connections)
  struct unix_sock *backlog_head;
  struct unix_sock *backlog_tail;
  int backlog_len;
  int backlog_max; // set by listen()

  // Shutdown state
  int shutdown_read;
  int shutdown_write;

  // Bind path (empty = unbound/anonymous)
  char sun_path[108];

  struct inode
      *bind_inode; /* VFS bind 路径：socket inode 引用（NULL=哈希表占名） */
  pid_t owner_pid; /* bind 进程 pid（VFS 路径用，对齐 bind_entry.owner_pid） */

  wait_queue_head *wq; // eager 分配（unix_sock_create 即 kmalloc），阻塞
                       // reader/epoll 等待者挂此
} unix_sock;

// ===================== Bind name space =====================
// Hash table: path -> unix_sock* (listener only, UNIX_LISTEN state)
#define UNIX_HASH_SIZE 64

typedef struct unix_bind_entry {
  char sun_path[108];
  struct unix_sock *sock;
  pid_t owner_pid;              // PID of the process that bound this socket
  struct unix_bind_entry *next; // chain for hash collisions
} unix_bind_entry;

// ===================== Global socket lock =====================
// Lock order: tasks_lock -> socket_lock -> scheduler_lock
extern spinlock socket_lock;

// ===================== Socket operations (internal) =====================
struct unix_sock *unix_sock_alloc(void);
void unix_sock_free(struct unix_sock *sock);
void unix_sock_release(struct unix_sock *sock);

struct sk_buff *skb_alloc(uint32_t data_len);
void skb_free(struct sk_buff *skb);
void skb_enqueue(struct unix_sock *sock, struct sk_buff *skb);
struct sk_buff *skb_dequeue(struct unix_sock *sock);

int unix_bind_lookup(const char *sun_path, struct unix_sock **out,
                     pid_t *owner_pid);
int unix_bind_register(const char *sun_path, struct unix_sock *sock,
                       pid_t owner_pid);
void unix_bind_unregister(struct unix_sock *sock);

int64_t unix_sock_sendmsg(struct unix_sock *sock, const struct iovec *iov,
                          size_t iovlen, const void *control, size_t controllen,
                          int flags);
int64_t unix_sock_recvmsg(struct unix_sock *sock, const struct iovec *iov,
                          size_t iovlen, void *control, size_t *controllen,
                          int flags);

// Syscall implementations
int64_t sys_socket(int64_t arg1, int64_t arg2, int64_t arg3, int64_t, int64_t,
                   int64_t);
int64_t sys_bind(int64_t arg1, int64_t arg2, int64_t arg3, int64_t, int64_t,
                 int64_t);
int64_t sys_listen(int64_t arg1, int64_t arg2, int64_t, int64_t, int64_t,
                   int64_t);
int64_t sys_accept(int64_t arg1, int64_t arg2, int64_t arg3, int64_t, int64_t,
                   int64_t);
int64_t sys_connect(int64_t arg1, int64_t arg2, int64_t arg3, int64_t, int64_t,
                    int64_t);
int64_t sys_socketpair(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4,
                       int64_t, int64_t);
int64_t sys_sendmsg(int64_t arg1, int64_t arg2, int64_t arg3, int64_t, int64_t,
                    int64_t);
int64_t sys_recvmsg(int64_t arg1, int64_t arg2, int64_t arg3, int64_t, int64_t,
                    int64_t);
int64_t sys_shutdown(int64_t arg1, int64_t arg2, int64_t, int64_t, int64_t,
                     int64_t);
int64_t sys_poll(int64_t arg1, int64_t arg2, int64_t arg3, int64_t, int64_t,
                 int64_t);

// Internal helpers for unix_sock_write/unix_sock_read
int64_t unix_sock_write(struct unix_sock *sock, const void *buf, size_t len);
int64_t unix_sock_read(struct unix_sock *sock, void *buf, size_t len);
void unix_sock_wake_reader(struct unix_sock *sock);
void unix_sock_wake_writer(struct unix_sock *sock);
void unix_sock_close(struct unix_sock *sock);

// recvfrom / sendto thin wrappers
int64_t sys_recvfrom(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_sendto(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);

// Socket name/option thin wrappers (F group)
int64_t sys_getsockname(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_getpeername(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_setsockopt(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
int64_t sys_getsockopt(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);

#endif // KERNEL_SOCKET_H

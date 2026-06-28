#ifndef KERNEL_SOCKET_H
#define KERNEL_SOCKET_H

#include <stdint.h>
#include "kernel/list.h"
#include "kernel/proc.h"
#include "kernel/spinlock.h"
#include "kernel/atomic.h"
#include "common/socket.h"

// ===================== sk_buff (socket buffer) =====================
// One per sendmsg call. Flexible array member for data.
#define MAX_SOCKET_DATA 65536  // 64KB soft limit (same as sys_msg)
#define MAX_SCM_FDS     8      // max fd count per SCM_RIGHTS

typedef struct sk_buff {
    struct sk_buff *next;       // linked list next
    uint32_t len;               // data length
    uint32_t consumed;          // bytes already read (SOCK_STREAM partial read)
    int      num_fds;           // number of SCM_RIGHTS fds
    int      fds[MAX_SCM_FDS];  // SCM_RIGHTS fd numbers (lazy install)
    uint8_t  data[];            // flexible array member
} sk_buff_t;

// allocate: skb = kmalloc(sizeof(sk_buff) + data_len)
// free: kfree(skb)

// ===================== unix_sock state =====================
#define UNIX_MAX_BACKLOG 8

typedef enum unix_sock_state {
    UNIX_FREE = 0,
    UNIX_LISTEN,
    UNIX_CONNECTED,
    UNIX_CLOSED,
} unix_sock_state_t;

// ===================== unix_sock (per-socket kernel structure) =====================
typedef struct unix_sock {
    int      state;                   // UNIX_* state
    pid_t    peer;                    // peer PID (CONNECTED)
    struct unix_sock *peer_sock;      // direct pointer to peer socket (socketpair/connect)
    refcount_t u_count;               // fd ref count (dup2 sharing)

    // Receive queue (skb linked list)
    struct sk_buff *recv_queue_head;
    struct sk_buff *recv_queue_tail;
    int    recv_queue_len;
    pid_t  blocked_reader;            // PID blocked in recvmsg/read (-1 = none)
    pid_t  blocked_writer;            // PID blocked in sendmsg/write (-1 = none)

    // Listen backlog (un-accepted connections)
    struct unix_sock *backlog_head;
    struct unix_sock *backlog_tail;
    int    backlog_len;
    int    backlog_max;               // set by listen()

    // Shutdown state
    int    shutdown_read;
    int    shutdown_write;

    // Bind path (empty = unbound/anonymous)
    char   sun_path[108];
} unix_sock_t;

// ===================== Bind name space =====================
// Hash table: path -> unix_sock* (listener only, UNIX_LISTEN state)
#define UNIX_HASH_SIZE 64

typedef struct unix_bind_entry {
    char   sun_path[108];
    struct unix_sock *sock;
    struct unix_bind_entry *next;  // chain for hash collisions
} unix_bind_entry_t;

// ===================== Global socket lock =====================
// Lock order: tasks_lock -> socket_lock -> scheduler_lock
extern spinlock_t socket_lock;

// ===================== Socket operations (internal) =====================
struct unix_sock *unix_sock_alloc(void);
void unix_sock_free(struct unix_sock *sock);
void unix_sock_acquire(struct unix_sock *sock);
void unix_sock_release(struct unix_sock *sock);

struct sk_buff *skb_alloc(uint32_t data_len);
void skb_free(struct sk_buff *skb);
void skb_enqueue(struct unix_sock *sock, struct sk_buff *skb);
struct sk_buff *skb_dequeue(struct unix_sock *sock);

int  unix_bind_lookup(const char *sun_path, struct unix_sock **out);
int  unix_bind_register(const char *sun_path, struct unix_sock *sock);
void unix_bind_unregister(struct unix_sock *sock);

int64_t sock_sendmsg_internal(struct unix_sock *sock,
                               const struct iovec *iov, size_t iovlen,
                               const void *control, size_t controllen,
                               int flags);
int64_t sock_recvmsg_internal(struct unix_sock *sock,
                               const struct iovec *iov, size_t iovlen,
                               void *control, size_t *controllen,
                               int flags);

// Syscall implementations
int64_t sys_socket(int64_t arg1, int64_t arg2, int64_t arg3, int64_t, int64_t, int64_t);
int64_t sys_bind(int64_t arg1, int64_t arg2, int64_t arg3, int64_t, int64_t, int64_t);
int64_t sys_listen(int64_t arg1, int64_t arg2, int64_t, int64_t, int64_t, int64_t);
int64_t sys_accept(int64_t arg1, int64_t arg2, int64_t arg3, int64_t, int64_t, int64_t);
int64_t sys_connect(int64_t arg1, int64_t arg2, int64_t arg3, int64_t, int64_t, int64_t);
int64_t sys_socketpair(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4, int64_t, int64_t);
int64_t sys_sendmsg(int64_t arg1, int64_t arg2, int64_t arg3, int64_t, int64_t, int64_t);
int64_t sys_recvmsg(int64_t arg1, int64_t arg2, int64_t arg3, int64_t, int64_t, int64_t);
int64_t sys_shutdown(int64_t arg1, int64_t arg2, int64_t, int64_t, int64_t, int64_t);
int64_t sys_poll(int64_t arg1, int64_t arg2, int64_t arg3, int64_t, int64_t, int64_t);

// Internal helpers for sock_write/sock_read
void sock_wake_reader(struct unix_sock *sock);
void sock_wake_writer(struct unix_sock *sock);
void sock_close(struct unix_sock *sock);

// Forward declaration from trap.c
void wake_process(pid_t pid);

#endif // KERNEL_SOCKET_H

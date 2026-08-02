/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_DRIVER_BSD_TYPES_H
#define KERNEL_DRIVER_BSD_TYPES_H

// BSD-layer types needed by driver dev_ops callbacks.
// Driver code must NOT include kernel/bsd/ headers directly (except
// devtmpfs.h). This header provides the minimal type definitions required by
// driver callbacks.
//
// If kernel/bsd/types.h is already included (e.g., by BSD-layer code),
// its guard KERNEL_BSD_TYPES_H prevents duplicate definitions here.

#include "kernel/xcore/atomic.h"
#include "kernel/xcore/mm_types.h" // mm, mmap_region, shm
#include "kernel/xcore/rcu.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/wait_queue.h"
#include "kernel/xcore/xtask.h" // xtask
#include <stddef.h>
#include <stdint.h>
#include <xos/signal.h> // NSIG, sigset_t, sigaction, siginfo_t
#include <xos/types.h>  // pid_t

// ===================== file / files =====================
// Must match kernel/bsd/types.h exactly.

#ifndef KERNEL_BSD_TYPES_H

#define MAX_FD 1024
#define FD_CLOEXEC 0x8000

#define FD_NONE 0
#define FD_PIPE 1
#define FD_REGULAR 2
#define FD_DEV 3
#define FD_DIR 4
#define FD_SOCKET 5
#define FD_SHM 6
#define FD_FILE 7
#define FD_TTY 8
#define FD_EPOLL 9
#define FD_EVENTFD 10
#define FD_TIMERFD 11
#define FD_SIGNALFD 12
#define FD_NETLINK 13
#define FD_IPC 14
#define FD_SYNC_FILE 15

struct inode;
struct unix_sock;
struct pty;
struct eventpoll;
struct eventfd_ctx;
struct timerfd_ctx;
struct signalfd_ctx;
struct netlink_sock;
struct drm_fence;
struct file_operations;

typedef struct file {
  refcount_t f_count;
  int type;
  int flags;
  struct inode *inode;
  uint64_t offset;
  wait_queue_head *wq;
  const struct file_operations *f_op;
  void *private_data;
  pid_t f_owner;
  int f_owner_sig;
  int f_owner_type;
  union {
    struct pipe *pipe;
    struct shm *shm;
    pid_t target_pid;
    struct {
      pid_t fs_pid;
      int32_t fs_fd;
      uint64_t _offset;
      uint64_t file_size;
      refcount_t f_count;
    } file_data;
    struct unix_sock *sock;
    struct pty *pty;
    struct eventpoll *epoll;
    struct eventfd_ctx *eventfd;
    struct timerfd_ctx *timerfd;
    struct signalfd_ctx *signalfd;
    struct netlink_sock *nlsock;
    pid_t ipcfd_owner_pid;
    struct drm_fence *sync_file_fence;
  };
} file;

typedef struct files {
  spinlock fd_lock;
  struct file *fd_table[MAX_FD];
  uint64_t close_on_exec[(MAX_FD + 63) / 64]; // S06: mirrors kernel/bsd/types.h
  refcount_t f_count;
} files;

#endif /* KERNEL_BSD_TYPES_H */

// ===================== proc =====================
// Must match kernel/bsd/proc.h exactly.
// Driver callbacks need proc->proc->files to access the fd table.
//
// The layout below must stay byte-for-byte identical to kernel/bsd/proc.h.
// In particular, `signal` is a POINTER to a separately-allocated signal_struct
// (NOT an inline struct) — inlining it here would shift the offset of `files`
// and cause out-of-bounds reads in driver callbacks.

#ifndef KERNEL_BSD_PROC_H

struct signal_struct;

typedef struct proc {
  struct xtask *xtask;

  int32_t exit_code;
  pid_t sid;
  pid_t pgid;
  struct pty *ctty;

  uint64_t sig_pending;
  sigset_t sig_blocked;
  siginfo_t sig_force_info;
  struct signal_struct *signal;

  struct files *files;

  void *clear_tid_addr; // S03: 64-bit (mirror kernel/bsd/proc.h)
  list_node futex_node;
  uint64_t futex_uaddr;

  // === pthread cancel (Phase 4) ===
  uint64_t cancel_handler; // __pthread_cancel_check function address, 0 = not
                           // registered

  // === POSIX identity & permissions (group 1-2) — mirror kernel/bsd/proc.h
  // ===
  uint32_t uid;
  uint32_t euid;
  uint32_t suid; // S19: saved-set UID (mirror kernel/bsd/proc.h)
  uint32_t gid;
  uint32_t egid;
  uint32_t sgid; // S19: saved-set GID (mirror kernel/bsd/proc.h)
  uint32_t umask;
  uint8_t exit_signal; // S19: clone exit signal (mirror kernel/bsd/proc.h)

  // === Working directory (mirror kernel/bsd/proc.h) ===
  char cwd[256];

  // === robust-futex list (mirror kernel/bsd/proc.h) ===
  void *robust_list_head;
  size_t robust_list_len;
} proc;

// ABI drift guard: must match kernel/bsd/proc.h byte-for-byte.
// If this assert fails, the driver-side proc copy has drifted from the
// canonical definition in kernel/bsd/proc.h — fix this struct to match.
// The numbers are duplicated here on purpose: if either side changes without
// the other, BOTH files fail to compile, which is impossible to miss.
#define DRV_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
DRV_STATIC_ASSERT(offsetof(proc, files) == 184,
                  "driver proc.files offset drift");
DRV_STATIC_ASSERT(offsetof(proc, signal) == 176,
                  "driver proc.signal must be POINTER not inline");
DRV_STATIC_ASSERT(sizeof(proc) == 536, "driver proc size drift");
#undef DRV_STATIC_ASSERT

#endif /* KERNEL_BSD_PROC_H */

// ===================== file refcount =====================
// Must match kernel/bsd/types.h.

#ifndef KERNEL_BSD_TYPES_H
void file_put(struct file *f);
static inline void file_get(struct file *f) { refcount_inc(&f->f_count); }
#endif

// ===================== fd lookup =====================
// Inline helper (same as kernel/bsd/types.h version).

#ifndef KERNEL_BSD_TYPES_H
static inline struct file *fd_lookup(files *files, int fd) {
  return RCU_DEREFERENCE(files->fd_table[fd]);
}
#endif

#endif /* KERNEL_DRIVER_BSD_TYPES_H */

#ifndef KERNEL_DRIVER_BSD_TYPES_H
#define KERNEL_DRIVER_BSD_TYPES_H

// BSD-layer types needed by driver dev_ops callbacks.
// Driver code must NOT include kernel/bsd/ headers directly (except devtmpfs.h).
// This header provides the minimal type definitions required by driver callbacks.
//
// If kernel/bsd/types.h is already included (e.g., by BSD-layer code),
// its guard KERNEL_BSD_TYPES_H prevents duplicate definitions here.

#include <stdint.h>
#include <stddef.h>
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/rcu.h"
#include "kernel/xcore/spinlock.h"
#include <xos/fcntl.h>
#include <xos/types.h>           // pid_t
#include <xos/signal.h>          // NSIG, sigset_t, sigaction, siginfo_t
#include "kernel/xcore/xtask.h"     // xtask_t
#include "kernel/xcore/mm_types.h"  // mm_t, mmap_region_t, shm_t

// ===================== file / files_t =====================
// Must match kernel/bsd/types.h exactly.

#ifndef KERNEL_BSD_TYPES_H

#define MAX_FD       32
#define FD_CLOEXEC 0x8000

#define FD_NONE    0
#define FD_PIPE    1
#define FD_REGULAR 2
#define FD_DEV     3
#define FD_DIR     4
#define FD_SOCKET  5
#define FD_SHM     6
#define FD_FILE    7
#define FD_TTY     8

struct inode;
struct unix_sock;
struct pty;

typedef struct file {
    refcount_t f_count;
    int type;
    int flags;
    struct inode *inode;
    uint64_t offset;
    union {
        struct pipe *pipe;
        struct shm  *shm;
        pid_t target_pid;
        struct {
            pid_t   fs_pid;
            int32_t fs_fd;
            uint64_t _offset;
            uint64_t file_size;
            refcount_t f_count;
        } file_data;
        struct unix_sock *sock;
        struct pty *pty;
    };
} file_t;

typedef struct files_t {
    spinlock_t fd_lock;
    struct file *fd_table[MAX_FD];
    refcount_t f_count;
} files_t;

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
    struct xtask_t *xtask;

    int32_t exit_code;
    pid_t sid;
    pid_t pgid;
    struct pty *ctty;

    uint64_t      sig_pending;
    sigset_t      sig_blocked;
    siginfo_t     sig_force_info;
    struct signal_struct *signal;

    struct files_t *files;

    pid_t    clear_tid_addr;
    list_node_t futex_node;
    uint64_t futex_uaddr;

    // === pthread cancel (Phase 4) ===
    uint64_t cancel_handler;   // __pthread_cancel_check 函数地址，0 = 未注册
} proc_t;

// ABI drift guard: must match kernel/bsd/proc.h byte-for-byte.
// If this assert fails, the driver-side proc_t copy has drifted from the
// canonical definition in kernel/bsd/proc.h — fix this struct to match.
// The numbers are duplicated here on purpose: if either side changes without
// the other, BOTH files fail to compile, which is impossible to miss.
#define DRV_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
DRV_STATIC_ASSERT(offsetof(proc_t, files) == 184, "driver proc_t.files offset drift");
DRV_STATIC_ASSERT(offsetof(proc_t, signal) == 176, "driver proc_t.signal must be POINTER not inline");
DRV_STATIC_ASSERT(sizeof(proc_t) == 232, "driver proc_t size drift");
#undef DRV_STATIC_ASSERT

#endif /* KERNEL_BSD_PROC_H */

// ===================== fd lookup =====================
// Inline helper (same as kernel/bsd/types.h version).

#ifndef KERNEL_BSD_TYPES_H
static inline struct file *fd_lookup(files_t *files, int fd) {
    return rcu_dereference(files->fd_table[fd]);
}
#endif

#endif /* KERNEL_DRIVER_BSD_TYPES_H */

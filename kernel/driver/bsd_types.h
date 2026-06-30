#ifndef KERNEL_DRIVER_BSD_TYPES_H
#define KERNEL_DRIVER_BSD_TYPES_H

// BSD-layer types needed by driver dev_ops callbacks.
// Driver code must NOT include kernel/bsd/ headers directly (except devtmpfs.h).
// This header provides the minimal type definitions required by driver callbacks.
//
// If kernel/bsd/types.h is already included (e.g., by BSD-layer code),
// its guard KERNEL_BSD_TYPES_H prevents duplicate definitions here.

#include <stdint.h>
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/rcu.h"
#include "kernel/xcore/spinlock.h"
#include "common/fcntl.h"
#include "common/types.h"           // pid_t
#include "common/signal.h"          // NSIG, sigset_t, sigaction, siginfo_t
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

#ifndef KERNEL_BSD_PROC_H

typedef struct proc {
    struct xtask_t *xtask;

    int32_t exit_code;
    pid_t sid;
    pid_t pgid;
    struct pty *ctty;

    struct signal_state {
        uint64_t      pending;
        sigset_t      blocked;
        struct sigaction action[NSIG];
    } sig;
    siginfo_t sig_force_info;

    struct files_t *files;
} proc_t;

#endif /* KERNEL_BSD_PROC_H */

// ===================== fd lookup =====================
// Inline helper (same as kernel/bsd/types.h version).

#ifndef KERNEL_BSD_TYPES_H
static inline struct file *fd_lookup(files_t *files, int fd) {
    return rcu_dereference(files->fd_table[fd]);
}
#endif

#endif /* KERNEL_DRIVER_BSD_TYPES_H */

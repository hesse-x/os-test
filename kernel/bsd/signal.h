#ifndef KERNEL_BSD_SIGNAL_H
#define KERNEL_BSD_SIGNAL_H

#include <stdint.h>
#include "common/signal.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/spinlock.h"

// Thread-group shared signal structure (ref counted)
// fork: independent copy; clone(CLONE_SIGHAND): ref++
struct signal_struct {
    refcount_t    sig_count;        // shared refcount (++ on CLONE_SIGHAND)
    atomic_t      thread_count;     // live threads in thread group (++ on CLONE_THREAD, -- on do_exit)
    atomic_t      live_count;       // threads still alive (not ZOMBIE), for waitpid thread-group check
    spinlock_t    sig_lock;         // protects shared_pending
    uint64_t      shared_pending;   // process-level pending (kill/pgsignal)
    sigaction_t   action[NSIG];     // handler table (shared across thread group)
    uint8_t       group_exit;       // exit_group flag
    int32_t       group_exit_code;  // exit_group exit code
    pid_t         parent_pid;       // thread group's parent PID (mirrored from mm_t.parent_pid to avoid UAF after mm freed)
};

struct signal_struct *signal_create(void);
void signal_put(struct signal_struct *sig);

#endif // KERNEL_BSD_SIGNAL_H

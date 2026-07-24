/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_BSD_SIGNAL_H
#define KERNEL_BSD_SIGNAL_H

#include <stdint.h>

#include "kernel/xcore/atomic.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/xtask.h" // xtask (do_cont / alarm_check prototypes)

#include <xos/signal.h>
#include <xos/types.h>

// Thread-group shared signal structure (ref counted)
// fork: independent copy; clone(CLONE_SIGHAND): ref++
struct signal_struct {
  refcount_t sig_count;  // shared refcount (++ on CLONE_SIGHAND)
  atomic_t thread_count; // live threads in thread group (++ on CLONE_THREAD, --
                         // on do_exit)
  atomic_t live_count;   // threads still alive (not ZOMBIE), for waitpid
                         // thread-group check
  spinlock sig_lock;     // protects shared_pending
  uint64_t shared_pending;  // process-level pending (kill/pgsignal)
  sigaction_t action[NSIG]; // handler table (shared across thread group)
  uint8_t group_exit;       // exit_group flag
  int32_t group_exit_code;  // exit_group exit code
  pid_t parent_pid; // thread group's parent PID — the authority for the
                    // parent relationship (waitpid child scan, orphan
                    // reparenting, getppid, SIGCHLD target). Stored per-process
                    // (not per-mm) so CLONE_VM children, which share their
                    // parent's mm, still resolve to the correct parent.
                    // Set at fork/clone to the caller's pid (or the caller's
                    // parent_pid under CLONE_PARENT/CLONE_THREAD).
  // S03: per-process alarm (POSIX alarm() is process-wide, shared by all
  // CLONE_THREAD threads). Replaces the per-task alarm_deadline that only
  // delivered SIGALRM to the arming thread. Read/written under sig_lock;
  // consumed by alarm_check_hook (BSD) invoked from the Xcore timer tick.
  uint64_t alarm_deadline; // 0 = no alarm; else sched_clock() ns absolute
};

struct signal_struct *signal_create(void);
void signal_put(struct signal_struct *sig);

// S01: job-control stop/continue. do_cont is called from sys_kill/sys_tgkill
// when a SIGCONT is delivered to a STOPPED target (resumes + clears the
// WUNTRACED one-shot report). Exported because the kill/tgkill delivery paths
// live in the same TU but are referenced from tests/debug.
void do_cont(xtask *t);

// signal_pending() has moved to kernel/xcore/xtask.h (Linux TIF_SIGPENDING
// model): a per-task cached boolean that drivers/xcore may test without a
// layering violation. The BSD signal path maintains it via recalc_sigpending()
// below — delivery, sigprocmask, rt_sigreturn, and the check_pending_signals
// drain all recalc so the cached bit tracks "any deliverable signal pending?".

// Recompute t->sig_pending (the xcore cached bit) from the live BSD state
// (private sig_pending ∪ thread-group shared_pending, then block-mask filtered
// with the SIGKILL/SIGSTOP override). Call at every point that changes pending
// bits or sig_blocked: after a delivery, after sigprocmask/rt_sigreturn, and
// after check_pending_signals consumes a signal. Mirrors Linux
// recalc_sigpending.
void recalc_sigpending(xtask *t);

// S03: per-process alarm expiry check (registered as the Xcore alarm_check_hook
// from bsd_init). If the current task's thread-group alarm_deadline has passed,
// force SIGALRM on the thread group and clear the deadline. Called every timer
// tick (IRQ context) and from timer-queue timeout wakes.
void alarm_check(xtask *t, uint64_t now);

// Thread syscalls (Phase 3a)
int64_t sys_tgkill(int64_t tgid, int64_t tid, int64_t sig, int64_t, int64_t,
                   int64_t);
int64_t sys_sigprocmask(int64_t how, int64_t set, int64_t oldset,
                        int64_t sigsetsize, int64_t, int64_t);
int64_t sys_set_tid_address(int64_t tidptr, int64_t, int64_t, int64_t, int64_t,
                            int64_t);
int64_t sys_arch_prctl(int64_t code, int64_t addr, int64_t, int64_t, int64_t,
                       int64_t);
int64_t sys_pthread_set_cancel_handler(int64_t handler, int64_t, int64_t,
                                       int64_t, int64_t, int64_t);
int64_t sys_sigpending(int64_t set, int64_t sigsetsize, int64_t, int64_t,
                       int64_t, int64_t);

#endif // KERNEL_BSD_SIGNAL_H

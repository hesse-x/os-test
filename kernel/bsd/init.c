/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// kernel/bsd/init.c — BSD layer initialization and hook registration
// Extracted from kernel/kernel.c (phase 5 step 5.3)

#include <stdbool.h>

#include "arch/x64/trap.h"
#include "kernel/bsd/devtmpfs.h"
#include "kernel/bsd/evdev_broker.h"
#include "kernel/bsd/file_fault.h"
#include "kernel/bsd/futex.h"
#include "kernel/bsd/netlink.h"
#include "kernel/bsd/proc.h"
#include "kernel/bsd/random.h"
#include "kernel/bsd/signal.h"
#include "kernel/bsd/syscall.h"
#include "kernel/bsd/timerfd.h"
#include "kernel/bsd/vfs.h"
#include "kernel/kernel.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/trap.h"
#include "kernel/xcore/xtask.h"

// Wrapper to adapt check_pending_signals(trapframe*) to
// signal_check_fn(xtask*, trapframe*) check_pending_signals uses
// current_task internally, so the xtask argument is redundant but required by
// the hook signature for future extensibility.
static void check_signals(xtask *task, trapframe *tf) {
  (void)task; // check_pending_signals reads current_task internally
  check_pending_signals(tf);
}

// Check if a signal is pending and deliverable for the given task
static bool check_signal_pending(xtask *t) { return signal_pending(t); }

void bsd_init(void) {
  vfs_init();
  printk(LOG_INFO, "bsd_init: vfs_init done\n");
  nl_init();
  printk(LOG_INFO, "bsd_init: nl_init done\n");

  // futex_table initialization (64 buckets + locks)
  for (int i = 0; i < FUTEX_HASH_SIZE; i++) {
    list_init(&futex_table[i].waiters);
    futex_table[i].lock = SPINLOCK_INIT;
  }

  // Register hooks: BSD layer provides implementations, Xcore calls them at
  // trap/syscall return
  signal_check_hook = check_signals;
  reap_hook = proc_reap_idle;
  proc_reap_hook = proc_reap;
  devtmpfs_cleanup_hook = devtmpfs_cleanup_pid;
  syscall_dispatch_hook = syscall_dispatch;
  signal_pending_hook = check_signal_pending;
  force_sig_hook = force_sig;
  // S03: per-process alarm expiry (replaces the per-task alarm sweep in the
  // Xcore timer handler).
  alarm_check_hook = alarm_check;
  // timerfd: initialize global list + lock, register tick sweep hook
  timerfd_init();
  timerfd_tick_hook = timerfd_tick_all;
  // evdev broker: create /dev/input/control control node
  evdev_broker_init();
  // random: register /dev/random + /dev/urandom
  random_dev_init();
  // S12: file-backed mmap page-in (MAP_PRIVATE+fd demand fault + COW).
  fault_handler = file_fault_handler;

  printk(LOG_INFO, "bsd_init: done\n");
}

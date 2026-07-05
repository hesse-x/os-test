// kernel/bsd/init.c — BSD layer initialization and hook registration
// Extracted from kernel/kernel.c (phase 5 step 5.3)

#include "arch/x64/trap.h"
#include "kernel/bsd/devtmpfs.h"
#include "kernel/bsd/futex.h"
#include "kernel/bsd/proc.h"
#include "kernel/bsd/syscall.h"
#include "kernel/bsd/types.h"
#include "kernel/bsd/vfs.h"
#include "kernel/kernel.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/trap.h"
#include "kernel/xcore/xtask.h"
#include <stdbool.h>
#include <stdint.h>
#include <xos/signal.h>

// Wrapper to adapt check_pending_signals(trapframe_t*) to
// signal_check_fn(xtask_t*, trapframe_t*) check_pending_signals uses
// current_task internally, so the xtask_t argument is redundant but required by
// the hook signature for future extensibility.
static void check_signals(xtask_t *task, trapframe_t *tf) {
  (void)task; // check_pending_signals reads current_task internally
  check_pending_signals(tf);
}

// Check if a signal is pending and deliverable for the given task
static bool check_signal_pending(xtask_t *t) {
  if (!t->proc)
    return false;
  uint64_t pend = __atomic_load_n(&t->proc->sig_pending, __ATOMIC_ACQUIRE);
  uint64_t deliv = pend & ~t->proc->sig_blocked;
  deliv |= (pend & ((1ULL << SIGKILL) | (1ULL << SIGSTOP)));
  return deliv != 0;
}

void bsd_init(void) {
  vfs_init();
  printk(LOG_INFO, "bsd_init: vfs_init done\n");

  // futex_table 初始化（64 bucket + lock）
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
  // fault_handler = NULL;  // future: file-backed mmap page-in

  printk(LOG_INFO, "bsd_init: done\n");
}

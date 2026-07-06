/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/xcore/rcu.h"
#include "arch/x64/smp.h" // ncpu, current_task
#include "kernel/xcore/log.h"
#include "kernel/xcore/xtask.h"

rcu_state g_rcu_state;

void rcu_init(void) {
  atomic_set(&g_rcu_state.global_gen, 0);
  for (int i = 0; i < RCU_MAX_CPUS; i++)
    atomic_set(&g_rcu_state.cpu_gen[i], 0);
  g_rcu_state.writer_lock = SPINLOCK_INIT;
}

void synchronize_rcu(void) {
  spin_lock(&g_rcu_state.writer_lock);
  int new_gen = atomic_add_return(&g_rcu_state.global_gen, 1);
  int my_cpu = get_cpu_local()->cpu_id;
  // Immediately advance our own cpu_gen — the caller is outside any
  // RCU read-side critical section, so this CPU is already in a
  // quiescent state.  Without this, a busy-waiting synchronize_rcu
  // would never see its own cpu_gen advance (it never passes through
  // rcu_read_unlock while spinning).
  atomic_set(&g_rcu_state.cpu_gen[my_cpu], new_gen - 1);
  // Wait for all OTHER online CPUs to observe at least new_gen - 1
  for (int i = 0; i < ncpu; i++) {
    if (i == my_cpu)
      continue;
    int spins = 0;
    while (atomic_read(&g_rcu_state.cpu_gen[i]) < new_gen - 1) {
      __asm__ volatile("pause");
      if (++spins > 100000000) {
        // Stall watchdog: print each CPU's lag + current task to identify
        // which CPU isn't passing through grace periods and what it's running.
        // Provides an order of magnitude more information than a single-line
        // WARN_ON -- original Bug 6 required guessing "which CPU is stuck",
        // now it's given directly.  Observation point only, does not change
        // RCU semantics (still continue waiting).
        printk(LOG_WARN, "RCU stall: global_gen=%d waiter_cpu=%d\n",
               new_gen - 1, i);
        for (int c = 0; c < ncpu; c++) {
          int cg = atomic_read(&g_rcu_state.cpu_gen[c]);
          int lag = (new_gen - 1) - cg;
          xtask *t = cpu_locals[c]._cur_proc;
          const char *stname = t ? "???" : "idle";
          if (t) {
            stname = (t->state == RUNNING)   ? "RUNNING"
                     : (t->state == BLOCKED) ? "BLOCKED"
                     : (t->state == READY)   ? "READY"
                     : (t->state == ZOMBIE)  ? "ZOMBIE"
                                             : "other";
          }
          printk(LOG_WARN, "  cpu%d: cpu_gen=%d lag=%d task_pid=%d state=%s\n",
                 c, cg, lag, t ? (int)t->pid : -1, stname);
        }
        WARN_ON(1);
        spins = 0; // continue waiting after warning
      }
    }
  }
  spin_unlock(&g_rcu_state.writer_lock);
}

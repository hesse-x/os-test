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
  // Immediately advance our own cpu_gen to new_gen — the caller is outside any
  // RCU read-side critical section (all synchronize_rcu callers are writers
  // releasing objects, never nested in rcu_read_lock), so this CPU is already
  // in a quiescent state. Published at new_gen (not new_gen - 1) so a
  // concurrent writer on another CPU, which waits for cpu_gen >= its own
  // new_gen, sees this CPU as having passed this grace period. Without this, a
  // busy-waiting synchronize_rcu would never see its own cpu_gen advance (it
  // never passes through rcu_read_unlock while spinning).
  atomic_set(&g_rcu_state.cpu_gen[my_cpu], new_gen);
  // Release writer_lock before busy-wait.  The lock only serializes
  // the global_gen increment; holding it across the wait serializes
  // all callers and, on SMP with IF=0, creates deadlock (a CPU
  // spinning on writer_lock can never reach a quiescent state to
  // advance cpu_gen).  Releasing early allows concurrent callers to
  // wait for different generation thresholds — all satisfied by the
  // same rcu_quiescent() calls from timer IRQs.
  spin_unlock(&g_rcu_state.writer_lock);
  // Wait for all OTHER online CPUs to publish a quiescent state at >= new_gen.
  //
  // Correctness (vs the old `>= new_gen - 1`): every reader critical section
  // disables IRQs (rcu_read_lock does `cli`), so while a CPU is inside a
  // read-side CS its cpu_gen cannot advance — the timer IRQ that would call
  // rcu_quiescent() cannot fire, and rcu_read_unlock() only publishes after the
  // CS ends. Therefore requiring cpu_gen[i] >= new_gen forces the writer to
  // wait until each CPU has either:
  //   (a) gone quiescent AFTER this writer incremented global_gen (timer IRQ on
  //       a user-mode/idle CPU published cpu_gen = global_gen >= new_gen), or
  //   (b) exited any read-side CS that was open at increment time (the
  //       rcu_read_unlock published cpu_gen = global_gen >= new_gen).
  // Both guarantee no reader on cpu i can still be dereferencing a pointer that
  // was valid before this grace period. The old `>= new_gen - 1` accepted a
  // quiescence published BEFORE the increment — a CPU that published cpu_gen=g,
  // returned to user mode, then entered a fresh read-side CS (rcu_read_lock
  // ingests no generation) was invisible to the writer, which then freed the
  // object the new reader was using (use-after-free → file_get BUG_ON, the
  // §2/§3/§4 panics). TCG's serialized vCPUs closed that window; KVM's true
  // parallel SMP opened it. See bug.md "★ 突破".
  for (int i = 0; i < ncpu; i++) {
    if (i == my_cpu)
      continue;
    int spins = 0;
    while (atomic_read(&g_rcu_state.cpu_gen[i]) < new_gen) {
      __asm__ volatile("pause");
      if (++spins > 100000000) {
        // Stall watchdog: print each CPU's lag + current task to identify
        // which CPU isn't passing through grace periods and what it's running.
        // Provides an order of magnitude more information than a single-line
        // WARN_ON -- original Bug 6 required guessing "which CPU is stuck",
        // now it's given directly.  Observation point only, does not change
        // RCU semantics (still continue waiting).
        printk(LOG_WARN, "RCU stall: global_gen=%d waiter_cpu=%d\n", new_gen,
               i);
        for (int c = 0; c < ncpu; c++) {
          int cg = atomic_read(&g_rcu_state.cpu_gen[c]);
          int lag = new_gen - cg;
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
}

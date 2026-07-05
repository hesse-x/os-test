/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_XCORE_SCHED_H
#define KERNEL_XCORE_SCHED_H

#include "arch/x64/apic.h"
#include "arch/x64/smp.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/mem/slab.h" // kmem_cache (needed for xtask_cache declaration)
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/xtask.h"
#include <stdint.h>

// Scheduler and process table (kernel/xcore/sched.c)

xtask *xtask_alloc(pid_t *out_pid);
void xtask_free(xtask *t);
void sched_init(void);
void schedule(void);
void sched_task_reap(xtask *proc);
xtask *sched_create_idle_process(int cpu_id);
void sched_idle_entry(void);
void sched_try_steal_task(void);
void sched_timer_queue_insert(int cpu, xtask *proc);
void sched_timer_queue_remove(xtask *proc);

// xtask-dedicated slab cache (defined in kernel/xcore/sched.c).
// After dynamic allocation, xtask is allocated via kmem_cache_alloc from this
// cache; slot reuse frees back to this cache.  Failure rollback paths in
// proc_create.c / proc.c must kmem_cache_free(xtask_cache, ...) to reclaim
// the object.
extern kmem_cache *xtask_cache;

static inline void sched_timer_queue_cancel(xtask *proc) {
  if (proc->wait_deadline != 0) {
    sched_timer_queue_remove(proc);
    proc->wait_deadline = 0;
  }
}

static inline void wake_from_wait(xtask *p) {
  sched_timer_queue_cancel(p);
  p->state = READY;
  p->wait_event = WAIT_NONE;
  p->wait_timed_out = 0;
  int cpu = p->assigned_cpu;
  list_push_back(&cpu_locals[cpu].run_queue, &p->run_node);
  cpu_locals[cpu].run_count++;

  // Reschedule IPI: set need_resched on target CPU's current task
  // (target is BLOCKED, not running; curr is what's actually running on that
  // CPU)
  xtask *curr = cpu_locals[cpu]._cur_proc;
  if (curr != p) {
    curr->need_resched = 1;
    int my_cpu = get_cpu_local()->cpu_id;
    if (cpu != my_cpu) {
      lapic_send_reschedule(cpu);
    }
  }
}

// wake_with_event: hold target's scheduler_lock; only wake if target is BLOCKED
// and wait_event == expected_event.  Mismatched event is no-op (lost wake-up
// safe: target was already woken by another path or not waiting on this event).
// Call outside bucket lock / socket_lock.  Consolidates repeated
// "hold scheduler_lock + check + wake_from_wait" pattern from futex.c / ipc.c.
static inline void wake_with_event(xtask *target,
                                   wait_event expected_event) {
  int tcpu = target->assigned_cpu;
  uint64_t flags;
  spin_lock_irqsave(&cpu_locals[tcpu].scheduler_lock, &flags);
  if (target->state == BLOCKED && target->wait_event == expected_event) {
    wake_from_wait(target);
  }
  spin_unlock_irqrestore(&cpu_locals[tcpu].scheduler_lock, flags);
}

// wake_process_any: interrupt any blocking state (for signal delivery --
// signals must be able to interrupt WAIT_FUTEX/WAIT_CHILD/WAIT_PIPE, etc.).
// Difference from wake_process: wake_process has narrow semantics, only handles
// IPC-class waits (PIPE/POLL/RECV), ASSERTs on other events;
// wake_process_any does not distinguish event type, unconditionally wakes any
// BLOCKED target.
static inline void wake_process_any(xtask *target) {
  int tcpu = target->assigned_cpu;
  uint64_t flags;
  spin_lock_irqsave(&cpu_locals[tcpu].scheduler_lock, &flags);
  if (target->state == BLOCKED) {
    wake_from_wait(target);
  }
  spin_unlock_irqrestore(&cpu_locals[tcpu].scheduler_lock, flags);
}

// Assembly entry points
void switch_to(xtask *prev, xtask *next);
void process_entry(void);

// Internal helpers
#define AFFINITY_THRESHOLD                                                     \
  1 // affinity threshold: prefer preferred CPU when its load <= min + this
#define RECHECK_THRESHOLD                                                      \
  2 // TOCTOU recheck threshold: consider switching CPU when queue has > this many waiters
int sched_pick_cpu(void);
int sched_pick_cpu_pref(int pref_cpu);
uint64_t sched_build_kstack(uint64_t k_stack_top, uint64_t entry_rip);
// Variant allowing caller-supplied user rsp (e.g. for argc/argv/auxv stack
// layout)
uint64_t sched_build_kstack_user_rsp(uint64_t k_stack_top, uint64_t entry_rip,
                               uint64_t user_rsp);

// ===================== FPU state save/restore =====================
// fxsave/fxrstor are FPU instructions; they trigger #NM when CR0.TS=1.
// In eager mode TS is always 0, so clts is a redundant no-op, but retained
// as defense (if TS is accidentally set, fxsave/fxrstor would nest #NM -> #DF).
// These two helpers are the only entry points for FPU state manipulation,
// structurally preventing "forgot clts".
static inline void kernel_fpu_save(void *buf) {
  __asm__ volatile("clts\n\t"
                   "fxsave (%0)" ::"r"(buf)
                   : "memory");
}

static inline void kernel_fpu_restore(void *buf) {
  __asm__ volatile("clts\n\t"
                   "fxrstor (%0)" ::"r"(buf)
                   : "memory");
}

void fpu_lazy_switch(xtask *t);
void fpu_context_switch(xtask *prev, xtask *next);

// Allocate an FPU state page and initialize it as a valid fxsave image
// (memset 0 + MXCSR=0x1F80).  Returns 1 on success / 0 on failure
// (bfc_alloc_page failed).  Caller is responsible for failure rollback.
int xcore_fpu_alloc(xtask *t);

#endif // KERNEL_XCORE_SCHED_H

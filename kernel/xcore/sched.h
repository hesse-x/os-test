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
#include "kernel/xcore/log.h"
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

// run_queue_push: the ONLY entry point to enqueue a task's run_node onto a
// CPU's run_queue.  Asserts single-tenancy (run_node must not already be on
// any run_queue).  Caller MUST hold cpu_locals[cpu].scheduler_lock.
// Idempotent-wake caveat (design1 §4.3): this assert catches class-A residual
// cross-source mis-wake AND dev-time pairing bugs, NOT the legal class-B wake
// race where a second wake lands before schedule() dequeues the node — that
// case must NOT route through the asserting path (see wake_from_wait below,
// which keeps the silent no-op for list_empty==false).
static inline void run_queue_push(int cpu, xtask *t) {
  ASSERT(
      list_empty(&t->run_node)); // single-tenancy: not on any queue before push
  list_push_back(&cpu_locals[cpu].run_queue, &t->run_node);
  cpu_locals[cpu].run_count++;
}

// run_queue_pop: the ONLY entry point to dequeue a task's run_node from its
// run_queue.  Asserts the node is currently linked, removes it, decrements the
// owning CPU's run_count, then re-inits to "not on any queue".  The owning CPU
// is t->assigned_cpu at call time — callers MUST NOT mutate assigned_cpu before
// the pop (sched_try_steal_task pops under v's lock while assigned_cpu==v, then
// reassigns).  Caller MUST hold the scheduler_lock of the CPU that owns the
// run_queue the node is on.
static inline void run_queue_pop(xtask *t) {
  ASSERT(!list_empty(&t->run_node)); // must be on a queue before pop
  list_remove(&t->run_node);
  list_init(&t->run_node); // reset to "not on any queue"
  cpu_locals[t->assigned_cpu].run_count--;
}

// timer_queue_wait_push: the ONLY entry point to insert a task's wait_node
// into a CPU's timer_queue.  Asserts single-tenancy (wait_node not already on
// a timer_queue).  Caller MUST hold cpu_locals[cpu].scheduler_lock.
// NOTE: differs from run_queue_push — sched_timer_queue_insert does sorted
// insertion by wait_deadline, so this wrapper delegates to the sorted insert
// after asserting the precondition.
static inline void timer_queue_wait_push(int cpu, xtask *t) {
  ASSERT(list_empty(&t->wait_node)); // single-tenancy: not on any timer_queue
  sched_timer_queue_insert(cpu, t);
}

// timer_queue_wait_pop: the ONLY entry point to remove a task's wait_node
// from its timer_queue.  Asserts linked, removes, re-inits to "not queued".
// Caller MUST hold the scheduler_lock of the CPU owning the timer_queue.
static inline void timer_queue_wait_pop(xtask *t) {
  ASSERT(!list_empty(&t->wait_node)); // must be on a timer_queue before pop
  list_remove(&t->wait_node);
  list_init(&t->wait_node); // reset to "not on any timer_queue"
}

// sched_arm_timed_wait: arm a timed blocking wait on behalf of an IPC caller.
// Owns the entire arm critical section: take proc->assigned_cpu's
// scheduler_lock -> re-check *replied_flag (lost-wake guard) -> either set
// wait_deadline + BLOCKED + event + timer_queue_wait_push, or clear
// wait_deadline (abort, never armed).  Returns true iff the caller went
// BLOCKED and must call schedule().
//
// The caller NEVER touches wait_deadline directly — deadline_ns is computed
// out of lock (sched_clock() + timeout) and applied under the lock only when
// we actually arm.  This structurally eliminates the "set deadline out of
// lock, forget to clear on aborted arm" class (design1 Q3 form-②).
//
// Pre-conditions: caller has reset the per-request fields
// (req_replied/msg_replied = 0, wait_timed_out = 0, reply buf/len, result)
// BEFORE calling; *replied_flag is published under this same scheduler_lock by
// the responder (sys_resp / sys_msg_resp), so the in-lock re-check is
// race-correct. Depends on design1 B1/B2: wait_node single-tenancy
// (timer_queue_wait_push asserts list_empty; prior wake/timeout re-inits
// wait_node).
static inline bool sched_arm_timed_wait(xtask *proc, wait_event event,
                                        uint64_t deadline_ns,
                                        uint8_t *replied_flag) {
  int cpu = proc->assigned_cpu;
  uint64_t flags;
  spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &flags);
  bool need_sleep = !(*replied_flag);
  if (need_sleep) {
    proc->wait_deadline = deadline_ns; // set BEFORE sorted insert
    proc->state = BLOCKED;
    proc->wait_event = event;
    timer_queue_wait_push(cpu, proc); // design1 B1 asserting variant
  } else {
    proc->wait_deadline = 0; // abort: absorb disarm semantics —
  } // clear a deadline whose timer was
    // never armed (form-①), not just a
    // defensive wipe of a stale value.
  spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, flags);
  return need_sleep;
}

static inline void wake_from_wait(xtask *p) {
  sched_timer_queue_cancel(p);
  p->state = READY;
  p->wait_event = WAIT_NONE;
  p->wait_timed_out = 0;
  int cpu = p->assigned_cpu;
  /* Idempotent enqueue: if run_node is already on a run_queue (a prior wake
     already enqueued p but schedule() hasn't dequeued it yet — possible when
     multiple wake sources race), do NOT push again.  A second list_push_back
     on a linked node rewires its prev/next and corrupts the run_queue ring.
     Just ensure state=READY and leave run_count untouched: p is already
     runnable and will be dequeued by the next schedule() on `cpu`. */
  if (list_empty(&p->run_node)) {
    run_queue_push(cpu, p);
  }
#ifndef NDEBUG
  else {
    // Class-B legal wake race (design1 §4.3/§7.3): a prior wake already
    // enqueued p but schedule() hasn't dequeued it yet.  This is expected;
    // the idempotent guard silently absorbs it.  Observe frequency only,
    // NEVER panic (a real second-push would be a class-A bug, but class-B
    // races are the common cause of list_empty==false here).
    printk(LOG_WARN, "wake_from_wait: idempotent no-op pid=%d (class-B race)\n",
           p->pid);
  }
#endif

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

// sched_cancel_spurious_wake: cancel a spurious wakeup that enqueued proc's
// run_node without a matching schedule() dequeue. Used by prepare_to_wait loops
// (ep_poll / sys_poll) on their early-return / break paths that do NOT go
// through schedule(): the loop marked state=BLOCKED at the top, a concurrent
// wake_wq_target may have hit that BLOCKED and pushed run_node into the
// run_queue (state=READY). If the re-check then finds the condition ready and
// breaks without calling schedule(), run_node would stay linked — a dangling
// node that trips run_queue_push's single-tenancy ASSERT on the next
// block+wake, and trips sched_try_steal_task's ASSERT(state==READY) (state was
// reset to RUNNING but the node remains). This cancels that: under proc's
// scheduler_lock, if run_node is still linked, pop it and force state=RUNNING.
// Idempotent: a no-op if no wake reached us (run_node empty) or if schedule()
// already dequeued it.
static inline void sched_cancel_spurious_wake(xtask *proc) {
  int cpu = proc->assigned_cpu;
  uint64_t flags;
  spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &flags);
  if (!list_empty(&proc->run_node)) {
    run_queue_pop(proc); // removes from cpu's run_queue, list_init, run_count--
  }
  proc->state = RUNNING;
  proc->wait_event = WAIT_NONE;
  proc->wait_timed_out = 0;
  spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, flags);
}

// wake_with_event: hold target's scheduler_lock; only wake if target is BLOCKED
// and wait_event == expected_event.  Mismatched event is no-op (lost wake-up
// safe: target was already woken by another path or not waiting on this event).
// Call outside bucket lock / socket_lock.  Consolidates repeated
// "hold scheduler_lock + check + wake_from_wait" pattern from futex.c / ipc.c.
static inline void wake_with_event(xtask *target, wait_event expected_event) {
  int tcpu = target->assigned_cpu;
  uint64_t flags;
  spin_lock_irqsave(&cpu_locals[tcpu].scheduler_lock, &flags);
  if (target->state == BLOCKED && target->wait_event == expected_event) {
    wake_from_wait(target);
  }
  spin_unlock_irqrestore(&cpu_locals[tcpu].scheduler_lock, flags);
}

// wake_wq_target: hold target's scheduler_lock; wake if target is BLOCKED.
// 队列身份制下的资源就绪唤醒：不查 wait_event（task 在我 wq 上即唤醒）。
// wake_from_wait 内部已设 wait_event=WAIT_NONE，故无需额外设。
// 用于 wq 回调（如 virtio_gpu_wake_cb）：语义即"task 在我 wq 上就唤醒"，
// 是去 guard 的 wake_with_event。锁序同 wake_with_event：scheduler_lock 单锁。
static inline void wake_wq_target(xtask *target) {
  int tcpu = target->assigned_cpu;
  uint64_t flags;
  spin_lock_irqsave(&cpu_locals[tcpu].scheduler_lock, &flags);
  if (target->state == BLOCKED)
    wake_from_wait(target);
  spin_unlock_irqrestore(&cpu_locals[tcpu].scheduler_lock, flags);
}

// wake_process_any: interrupt any blocking state (for signal delivery --
// signals must be able to interrupt WAIT_FUTEX/WAIT_CHILD/WAIT_RECV, etc.).
// Does not distinguish event type, unconditionally wakes any BLOCKED target.
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
  2 // TOCTOU recheck threshold: consider switching CPU when queue has > this
    // many waiters
int sched_pick_cpu(void);
int sched_pick_cpu_pref(int pref_cpu);
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

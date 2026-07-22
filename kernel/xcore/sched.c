/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// kernel/xcore/sched.c — Scheduler and process table management
// Extracted from kernel/proc.c (phase 4 step 4.1)

#include <stddef.h>
#include <stdint.h>

#include "arch/x64/apic.h"
#include "arch/x64/memlayout.h"
#include "arch/x64/paging.h"
#include "arch/x64/smp.h"
#include "arch/x64/trap.h"
#include "arch/x64/utils.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/mm_types.h"
#include "kernel/xcore/rcu.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/trap.h"
#include "kernel/xcore/xtask.h"

#include <xos/page.h>
#include <xos/signal.h> // SS_DISABLE (init_xtask_defaults sigaltstack state)
#include <xos/syscall_nums.h>

// Validate assembly offset assumptions in trapentry.S (switch_to uses hardcoded
// offsets)
_Static_assert(offsetof(xtask, k_rsp) == 8,
               "switch_to asm: k_rsp offset mismatch");
_Static_assert(offsetof(xtask, cr3) == 24,
               "switch_to asm: cr3 offset mismatch");
_Static_assert(sizeof(trapframe) == 176,
               "trapframe size must be 176 (22 × uint64_t)");
_Static_assert(
    offsetof(cpu_local, tss_rsp0) == 48,
    "syscall_fast_entry asm: tss_rsp0 offset mismatch (expected 48)");

// fs_base offset consumed by trapentry.S (wrmsr FS_BASE before returning to
// user mode). Pinned by static_assert at the bottom of this file.
const uint64_t xtask_fs_base_offset = offsetof(xtask, fs_base);

xtask *tasks[MAX_PROC];
// current_task is per-CPU (in cpu_local), accessed via macro

spinlock tasks_lock = SPINLOCK_INIT;
pid_t init_pid = -1;
pid_t next_pid = 0; // incrementing + circular reuse (wraps at MAX_PROC)
kmem_cache *xtask_cache = NULL; // xtask-dedicated slab cache (dynamic)

// ===================== Process table =====================

// Allocate a free slot from tasks[] under tasks_lock.
// Returns pointer to the free xtask, or NULL if no free slot.
// Caller must hold tasks_lock; on success the slot's pid is still -1
// (caller sets it after allocating resources).
//
// reclaim_lazy_resources: release resources deferred by sched_task_reap.
//
// Deferred release list (SMP race protection) — these resources are still
// referenced in schedule()'s switch path.  sched_task_reap and schedule()
// share no lock; immediate free would cause UAF.  Instead, release them
// here when the slot is reused:
//   - k_stack (KERNEL_STACK_PAGES pages): switch_to asm executes ret on child's
//   stack
//   - fpu_page (1 page)  : fpu_context_switch fxsaves this page in prev branch
// By slot reuse time the child has long been switched away — safe.
// Add similar resources (e.g. extended FPU state pages) to this function.
static void reclaim_lazy_resources(xtask *t) {
  if (t->k_stack_top != 0) {
    uint64_t k_stack_phys_base =
        (__force uint64_t)PHY_ADDR(t->k_stack_top - KERNEL_STACK_SIZE);
    struct page *stack_page = &bfc_frames[PHY_TO_PAGE(k_stack_phys_base)];
    bfc_free_page(stack_page, KERNEL_STACK_PAGES);
    t->k_stack_top = 0;
  }
  if (t->fpu_page) {
    bfc_free_page(t->fpu_page, 1);
    t->fpu_page = NULL;
  }
}

// init_xtask_defaults: default field initialization for new xtask objects
// (zero + non-zero defaults + list node init).  Centralized here for
// xtask_alloc reuse, replacing the old per-slot init in sched_init.
// Note: does NOT set k_stack_top/fpu_page — they are set by the caller
// after allocating stack/FPU pages; slot reuse path clears them via
// reclaim_lazy_resources.
static void init_xtask_defaults(xtask *t) {
  __memset(t, 0, sizeof(*t));
  t->pid = -1;
  t->state = UNUSED;
  t->tgid = -1;
  t->assigned_cpu = -1;
  t->recv_lock = SPINLOCK_INIT;
  t->req_caller_pid = -1;
  t->req_target_pid = -1;
  t->req_replied = 0;
  t->msg_caller_pid = -1;
  t->msg_target_pid = -1;
  t->msg_replied = 0;
  list_init(&t->run_node);
  list_init(&t->wait_node);
  // S04: a fresh task has no alternate signal stack. memset clears
  // sas_ss_sp/size/flags to 0; make the disabled state explicit so the
  // SA_ONSTACK delivery path can distinguish "no altstack" from a valid one.
  t->sas_ss_flags = SS_DISABLE;
}

// xtask_alloc: scan from next_pid for an empty slot, allocate xtask,
// and write it to tasks[i].  Returns new xtask*, *out_pid = slot index
// (i.e. pid); returns NULL if no empty slot.
//
// Slot reuse semantics (aligned with decision #7 REAPING deferred release):
//   - tasks[i] == NULL          : kmem_cache_alloc new object directly
//   - tasks[i]->state == REAPING: reclaim_lazy_resources first (free
//                                  deferred k_stack/fpu_page), then
//                                  kmem_cache_free old object, alloc new
//   - other state               : slot occupied, skip
// Caller must hold tasks_lock.
xtask *xtask_alloc(pid_t *out_pid) {
  if (!xtask_cache)
    return NULL; // not initialized (called before sched_init)

  // Circular scan from next_pid (ffz semantics), find first reusable slot
  for (int k = 0; k < MAX_PROC; k++) {
    int i = (next_pid + k) % MAX_PROC;
    xtask *old = tasks[i];
    if (old == NULL) {
      // Empty slot: allocate directly
      xtask *t = (xtask *)kmem_cache_alloc(xtask_cache);
      if (!t)
        return NULL;
      init_xtask_defaults(t);
      tasks[i] = t;
      next_pid = (i + 1) % MAX_PROC;
      if (out_pid)
        *out_pid = (pid_t)i;
      return t;
    }
    if (old->state == REAPING) {
      // Exit handshake: do not reuse the slot until the dying CPU has
      // completed its final context switch off this task's kernel stack.
      // Before exit_done, the dying CPU may still be executing its do_exit
      // tail (parent notify / waiter-wake loop) and schedule()/switch_to on
      // this stack, and may still write this xtask object (switch_to's k_rsp
      // store, cpu-time accounting).  Reclaiming now would free the stack
      // under a running CPU and recycle the xtask object (same slab address)
      // under its late writes — bug.md §4 heap corruption.  Skip and let the
      // scan find another slot; the abandoning CPU sets exit_done at its
      // next schedule() entry (cpu_local.pending_dead finalize).
      if (!__atomic_load_n(&old->exit_done, __ATOMIC_ACQUIRE)) {
        continue;
      }
      // REAPING slot: reclaim deferred resources -> free old -> alloc new
      reclaim_lazy_resources(old);
      kmem_cache_free(xtask_cache, old);
      tasks[i] = NULL;
      xtask *t = (xtask *)kmem_cache_alloc(xtask_cache);
      if (!t)
        return NULL;
      init_xtask_defaults(t);
      tasks[i] = t;
      next_pid = (i + 1) % MAX_PROC;
      if (out_pid)
        *out_pid = (pid_t)i;
      return t;
    }
    // Slot occupied, continue scanning
  }
  return NULL; // full
}

void sched_init() {
  // Dynamic: create xtask-dedicated slab cache, init pointer array to NULL
  xtask_cache = kmem_cache_create("xtask", sizeof(xtask));
  if (!xtask_cache) {
    panic("sched_init: kmem_cache_create(xtask) failed\n");
  }
  for (int i = 0; i < MAX_PROC; i++) {
    tasks[i] = NULL;
  }
  cpu_locals[0]._cur_proc = NULL;
  cpu_locals[0].run_count = 0;
  cpu_locals[0].idle_proc = NULL;
  for (int c = 0; c < NUM_KMALLOC_CLASSES; c++) {
    cpu_locals[0].active_slab[c] = NULL;
  }
}

// switch_to restore frame: callee-saved registers + return address
typedef struct switch_frame {
  uint64_t rbx;
  uint64_t rbp;
  uint64_t r12;
  uint64_t r13;
  uint64_t r14;
  uint64_t r15;
  uint64_t ret_addr;
} switch_frame;

_Static_assert(sizeof(switch_frame) == 56,
               "switch_frame size must be 56 (7 × uint64_t)");

uint64_t sched_build_kstack_user_rsp(uint64_t k_stack_top, uint64_t entry_rip,
                                     uint64_t user_rsp) {
  trapframe tf = {0};
  tf.ss = 0x23; // USER_DS
  if (user_rsp == 0)
    user_rsp = 0x00007FFFFFFFE000ULL;
  tf.rsp = user_rsp; // user stack pointer
  // User stack top must be 16-byte aligned: iret enters _start with
  // rsp%16==0, _start's andq $-16 then satisfies ABI (call leaves
  // rsp%16==8).  Misalignment here causes user-space movaps/movdqa #GP.
  ASSERT(tf.rsp % 16 == 0);
  tf.rflags = 0x202; // IF=1, IOPL=0
  tf.cs = 0x2B;      // USER_CS
  tf.rip = entry_rip;
  tf.err_code = 0;
  tf.trapno = 0;

  switch_frame sf = {0};
  sf.ret_addr = (uint64_t)process_entry;

  uint8_t *sp = (uint8_t *)k_stack_top;
  sp -= sizeof(trapframe);
  __memcpy(sp, &tf, sizeof(trapframe));

  sp -= sizeof(switch_frame);
  __memcpy(sp, &sf, sizeof(switch_frame));

  return (uint64_t)sp;
}

// Build idle kernel stack: only switch_frame (no trapframe), ret_addr =
// sched_idle_entry
static uint64_t build_idle_kstack(uint64_t k_stack_top) {
  switch_frame sf = {0};
  sf.ret_addr = (uint64_t)sched_idle_entry;

  uint8_t *sp = (uint8_t *)k_stack_top;
  sp -= sizeof(switch_frame);
  __memcpy(sp, &sf, sizeof(switch_frame));

  return (uint64_t)sp;
}

// Create idle process for the specified CPU
xtask *sched_create_idle_process(int cpu_id) {
  spin_lock(&tasks_lock);
  pid_t alloc_idx = -1;
  xtask *proc = xtask_alloc(&alloc_idx);
  if (!proc) {
    spin_unlock(&tasks_lock);
    printk(LOG_ERROR, "sched_create_idle_process: no free slot\n");
    return NULL;
  }

  // Allocate kernel stack (KERNEL_STACK_SIZE)
  struct page *stack_pages = bfc_alloc_page(KERNEL_STACK_PAGES);
  if (!stack_pages) {
    spin_unlock(&tasks_lock);
    printk(LOG_ERROR, "sched_create_idle_process: alloc stack failed\n");
    return NULL;
  }
  uint64_t k_stack_phys = (__force uint64_t)page_to_phys(stack_pages);
  uint64_t k_stack_top =
      (__force uint64_t)phys_to_virt((__force phys_addr_t)k_stack_phys) +
      KERNEL_STACK_SIZE;

  // Build idle switch_frame on kernel stack (no trapframe, no user mode)
  uint64_t k_rsp = build_idle_kstack(k_stack_top);

  // Fill PCB: idle uses kernel PML4, no user address space
  proc->pid = alloc_idx;
  proc->state = RUNNING; // idle starts as RUNNING on its CPU
  proc->k_rsp = k_rsp;
  proc->k_stack_top = k_stack_top;
  proc->cr3 = (__force uint64_t)PHY_ADDR(
      (uintptr_t)pml4); // kernel PML4 physical address (cached)
  proc->entry = (uint64_t)sched_idle_entry;
  proc->wait_event = WAIT_NONE;
  proc->tgid = proc->pid;
  proc->mm = NULL; // idle has no address space
  proc->assigned_cpu = cpu_id;
  proc->iopm = NULL;
  proc->proc = NULL; // will be created by proc_create for user processes
  proc->cpu_time_ns = 0;
  proc->last_sched = 0;
  // POSIX fields are in proc (created separately)
  list_init(&proc->run_node);
  list_init(&proc->wait_node);
  spin_unlock(&tasks_lock);

  cpu_locals[cpu_id].idle_proc = proc;

  return proc;
}

__attribute__((no_sanitize("kernel-address"))) void sched_idle_entry() {
  sti();
  while (1) {
    // Idle is a quiescent state — report RCU quiescence so
    // synchronize_rcu() doesn't spin forever waiting for this CPU.
    rcu_read_lock();
    rcu_read_unlock();

    // Reap zombie processes (BSD hook for POSIX cleanup)
    if (reap_hook)
      reap_hook();

    // Work stealing: when this CPU is idle, try to steal a task from the
    // busiest CPU
    sched_try_steal_task();

    schedule();
    sti();
    __asm__ volatile("hlt");
  }
}

// sched_pick_cpu_pref: pick the least-loaded CPU, with optional preferred CPU
// (affinity). pref_cpu < 0 means no preference; otherwise the preferred CPU is
// chosen when its load is within AFFINITY_THRESHOLD of the minimum.
int sched_pick_cpu_pref(int pref_cpu) {
  int best = 0;
  int min = __atomic_load_n(&cpu_locals[0].run_count, __ATOMIC_RELAXED);
  for (int i = 1; i < ncpu; i++) {
    int r = __atomic_load_n(&cpu_locals[i].run_count, __ATOMIC_RELAXED);
    if (r < min) {
      min = r;
      best = i;
    }
  }
  if (pref_cpu >= 0 && pref_cpu < ncpu) {
    int pref_r =
        __atomic_load_n(&cpu_locals[pref_cpu].run_count, __ATOMIC_RELAXED);
    if (pref_r - min <= AFFINITY_THRESHOLD)
      best = pref_cpu;
  }
  return best;
}

// Pick the CPU with the fewest runnable processes
int sched_pick_cpu(void) { return sched_pick_cpu_pref(-1); }

// sched_try_steal_task: when this CPU is idle, steal a task from the tail of
// the busiest CPU's run_queue. The stealer holds its own CPU's scheduler_lock
// (to prevent concurrent interrupt writes to its own run_queue) and trylocks
// the target CPU's lock; on failure it gives up (opportunistic, never blocks).
// On success, update assigned_cpu = my_cpu.
void sched_try_steal_task(void) {
  int my_cpu = get_cpu_local()->cpu_id;
  uint64_t flags;
  spin_lock_irqsave(&cpu_locals[my_cpu].scheduler_lock, &flags);

  // Pick victim: linear scan for the maximum run_count
  int best_v = -1;
  int max_run = 0;
  for (int v = 0; v < ncpu; v++) {
    if (v == my_cpu)
      continue;
    int r = __atomic_load_n(&cpu_locals[v].run_count, __ATOMIC_RELAXED);
    if (r > max_run) {
      max_run = r;
      best_v = v;
    }
  }
  if (max_run <= 1)
    goto out; // nothing to steal (stealing would empty the target)

  int v = best_v;
  uint64_t vflags;
  if (!spin_trylock_irqsave(&cpu_locals[v].scheduler_lock, &vflags))
    goto out;

  // Re-check after entering the critical section: run_count is an external
  // snapshot, the queue may already be empty
  if (list_empty(&cpu_locals[v].run_queue)) {
    spin_unlock_irqrestore(&cpu_locals[v].scheduler_lock, vflags);
    goto out;
  }
  ASSERT(cpu_locals[v].run_count > 0);

  // Steal the tail: for a circular doubly-linked list head->prev is the tail
  // (list_push_back already relies on this invariant)
  list_node *head = &cpu_locals[v].run_queue;
  list_node *tail_node = head->prev;
  xtask *t = LIST_ENTRY(tail_node, xtask, run_node);
  // design1 §4.4: steal integrity asserts — the stolen task must be READY and
  // its run_node on the victim's run_queue (we hold v's scheduler_lock, so the
  // push side, which needs t->assigned_cpu's lock, cannot concurrently push).
  ASSERT(t->state == READY);
  ASSERT(t->assigned_cpu == v);
  run_queue_pop(t);         // removes from v's run_queue + list_init
  t->assigned_cpu = my_cpu; // mutate under v's lock (still held); push below
                            // uses my_cpu which we also hold — read/modify of
                            // assigned_cpu stays under a single lock window.
  run_queue_push(my_cpu, t);

  spin_unlock_irqrestore(&cpu_locals[v].scheduler_lock, vflags);
out:
  spin_unlock_irqrestore(&cpu_locals[my_cpu].scheduler_lock, flags);
}

// Update TSS IOPM for the current CPU to match the given process
static void update_tss_iopm(xtask *proc) {
  int cpu = get_cpu_local()->cpu_id;
  struct tss_struct *tss = &per_cpu_tss[cpu];
  if (proc->iopm) {
    __memcpy(tss->iopm, proc->iopm, IOPM_SIZE);
  } else {
    // Deny all ports
    for (int i = 0; i < IOPM_SIZE; i++)
      tss->iopm[i] = 0xFF;
  }
}

// FPU context switch C helper (eager mode, does not modify switch_to asm)
// Called before switch_to(prev, next): fxsave prev's FPU state + fxrstor
// next's FPU state. Does not set CR0.TS, user-space SSE instructions execute
// directly without triggering #NM. idle process has fpu_page=NULL, skipped
// automatically.
//
// UAF defense: fpu_page release is deferred (see sched_task_reap resource
// list), but if someone incorrectly frees it immediately in sched_task_reap,
// schedule() here would UAF. ASSERT page->status == PAGE_USED catches this
// immediately in DEBUG builds — bfc_free_page sets status to PAGE_FREE after
// freeing, exposing it before fxsave.
void fpu_context_switch(xtask *prev, xtask *next) {
  if (prev && prev->fpu_page) {
    ASSERT(prev->fpu_page->status == PAGE_USED);
    void *fpu_data =
        (void *)(__force uintptr_t)phys_to_virt(page_to_phys(prev->fpu_page));
    // Defense: fxsave target must be a BFC data page whose physical address
    // lies within managed RAM. Compare by physical page number, NOT by VA
    // upper bound: phys_to_virt(0) + total_page_frames*PAGE_SIZE wraps in
    // 64-bit on large RAM and would falsely fire (or, worse, a stale
    // VA-boundary check could pass a wild pointer). Pure physical comparison
    // holds for any RAM size. (Historically this also caught the bug where the
    // struct page * from bfc_alloc_page was mistaken for a data pointer.)
    // pphys is computed inline inside ASSERT so that release builds (NDEBUG →
    // ASSERT is a no-op) don't declare an unused variable.
    ASSERT((__force phys_addr_t)page_to_phys(prev->fpu_page) <
           (phys_addr_t)total_page_frames * PAGE_SIZE);
    kernel_fpu_save(fpu_data); // internally clts then fxsave
  }
  if (next && next->fpu_page) {
    ASSERT(next->fpu_page->status == PAGE_USED);
    void *fpu_data =
        (void *)(__force uintptr_t)phys_to_virt(page_to_phys(next->fpu_page));
    ASSERT((__force phys_addr_t)page_to_phys(next->fpu_page) <
           (phys_addr_t)total_page_frames * PAGE_SIZE);
    kernel_fpu_restore(fpu_data); // internally clts then fxrstor
  }
}

// Allocate an FPU state page and initialize it as a valid fxsave image.
// In the 512-byte fxsave layout MXCSR is at offset 24 and must be a valid
// value (otherwise fxrstor triggers #GP). memset 0 + setting MXCSR=0x1F80
// (default: all exception mask bits set, rounding=nearest) is equivalent to
// the init state after fninit+ldmxcsr. Pure memory operations, no SSE
// instructions, does not clobber caller's xmm registers.
int xcore_fpu_alloc(xtask *t) {
  t->fpu_page = bfc_alloc_page(1);
  if (!t->fpu_page)
    return 0;
  void *fpu_data =
      (void *)(__force uintptr_t)phys_to_virt(page_to_phys(t->fpu_page));
  __memset(fpu_data, 0, 512);
  *(uint32_t *)((uint8_t *)fpu_data + 24) = 0x1F80; // MXCSR default
  return 1;
}

__attribute__((no_sanitize("kernel-address"))) void schedule() {
  int my_cpu = get_cpu_local()->cpu_id;
  xtask *idle = get_cpu_local()->idle_proc;
  xtask *prev = current_task;

  uint64_t flags;
  spin_lock_irqsave(&cpu_locals[my_cpu].scheduler_lock, &flags);

  // Exit handshake finalize: if this CPU previously switched away from a
  // dying task (recorded as pending_dead at the switch-out below), that
  // switch has long completed — we are running here on a different stack.
  // Publish exit_done so xtask_alloc may reuse the REAPING slot (free the
  // dead kernel stack / recycle the xtask object).  Monotonic store; runs
  // under scheduler_lock with IRQs off, so no nested-schedule() race.
  xtask *pending_dead = get_cpu_local()->pending_dead;
  if (pending_dead) {
    get_cpu_local()->pending_dead = NULL;
    __atomic_store_n(&pending_dead->exit_done, 1, __ATOMIC_RELEASE);
  }

  prev->need_resched = 0; // consume flag: schedule() called = flag cleared

  // Check if run_queue has a runnable process
  if (list_empty(&cpu_locals[my_cpu].run_queue)) {
#ifndef NDEBUG
    ASSERT(cpu_locals[my_cpu].run_count == 0);
#endif
    // If prev is BLOCKED, ZOMBIE, or REAPING, it cannot continue running —
    // switch to idle so the CPU halts until an IRQ wakes a process.
    if (prev != idle && (prev->state == BLOCKED || prev->state == ZOMBIE ||
                         prev->state == REAPING)) {
#ifndef NDEBUG
      if (prev->state == BLOCKED) {
        printk(LOG_DEBUG, "schedule: pid=%d BLOCKED wait_event=%d\n", prev->pid,
               (int)prev->wait_event);
      }
#endif
      // Account prev's CPU time before switching to idle
      if (prev->last_sched != 0) {
        prev->cpu_time_ns += sched_clock() - prev->last_sched;
      }
      current_task = idle;
      per_cpu_tss[my_cpu].rsp0 = idle->k_stack_top;
      get_cpu_local()->tss_rsp0 = idle->k_stack_top;
      update_tss_iopm(idle);
      spin_unlock(&cpu_locals[my_cpu].scheduler_lock);
      ASSERT(get_cpu_local()->rcu.nesting ==
             0); // must not schedule inside RCU read-side CS
      // prev dies here: record so the next schedule() entry on this CPU can
      // publish exit_done (see xtask.exit_done).  The entry finalize above
      // guarantees pending_dead is NULL at this point.
      if (prev->state == ZOMBIE || prev->state == REAPING) {
        ASSERT(get_cpu_local()->pending_dead == NULL);
        get_cpu_local()->pending_dead = prev;
      }
      fpu_context_switch(prev, idle);
      switch_to(prev, idle);
      spin_lock(&cpu_locals[my_cpu].scheduler_lock);
      spin_unlock_irqrestore(&cpu_locals[my_cpu].scheduler_lock, flags);
      return;
    }
    spin_unlock_irqrestore(&cpu_locals[my_cpu].scheduler_lock, flags);
    return; // no runnable process, prev continues
  }

  // Dequeue next process from head (FIFO round-robin)
  list_node *next_node = list_front(&cpu_locals[my_cpu].run_queue);
  xtask *next = LIST_ENTRY(next_node, xtask, run_node);
  run_queue_pop(next);

  // Account prev's CPU time before switching out
  if (prev != idle && prev->last_sched != 0) {
    prev->cpu_time_ns += sched_clock() - prev->last_sched;
  }

  // State transition for prev
  if (prev != idle && prev->state == RUNNING) {
    prev->state = READY;
    run_queue_push(my_cpu, prev);
  }
  // if prev->state == BLOCKED, ZOMBIE, or REAPING: don't enqueue, run_count
  // unchanged

  next->state = RUNNING;
  next->last_sched = sched_clock();
  // run_queue_pop(next) above already decremented run_count[my_cpu] (pop is
  // the sole dequeue entry and now owns the counter); do NOT double-decrement.
#ifndef NDEBUG
  // Consistency check: after all run_queue / run_count changes are done, the
  // queue length must equal run_count. If a cross-CPU path writes to this
  // CPU's run_queue/run_count without holding this CPU's scheduler_lock, this
  // will catch it (previously timer_handler cross-CPU delivery was exposed
  // here).
  {
    int cnt = 0;
    list_node *__head = &cpu_locals[my_cpu].run_queue;
    list_node *__n = __head->next;
    while (__n != __head) {
      cnt++;
      __n = __n->next;
    }
    ASSERT(cnt == cpu_locals[my_cpu].run_count);
  }
#endif
  current_task = next;
  per_cpu_tss[my_cpu].rsp0 = next->k_stack_top;
  get_cpu_local()->tss_rsp0 = next->k_stack_top;
  update_tss_iopm(next);

  // Release lock but keep interrupts disabled — switch_to must run under cli
  // to prevent interrupt handlers from corrupting the stack during RSP/CR3
  // switch. After switch_to returns (prev is resumed on prev's stack),
  // re-acquire lock WITHOUT saving flags (spin_lock, not spin_lock_irqsave) so
  // the original flags saved before the context switch remain intact. Then
  // unlock+irqrestore using those original flags to correctly restore the
  // interrupt state.
  spin_unlock(&cpu_locals[my_cpu].scheduler_lock);
  ASSERT(get_cpu_local()->rcu.nesting ==
         0); // must not schedule inside RCU read-side CS
  // prev dies here: record so the next schedule() entry on this CPU can
  // publish exit_done (see xtask.exit_done).  The entry finalize above
  // guarantees pending_dead is NULL at this point.
  if (prev != idle && (prev->state == ZOMBIE || prev->state == REAPING)) {
    ASSERT(get_cpu_local()->pending_dead == NULL);
    get_cpu_local()->pending_dead = prev;
  }
  fpu_context_switch(prev, next);
  switch_to(prev, next);
  spin_lock(&cpu_locals[my_cpu].scheduler_lock);
  spin_unlock_irqrestore(&cpu_locals[my_cpu].scheduler_lock, flags);
}

// ===================== Timer queue operations =====================
// Must be called under scheduler_lock of the target CPU

// design1 §6.2: sched_timer_queue_insert is now the sorted-insert body only;
// single-tenancy assertion lives in the timer_queue_wait_push wrapper.  The
// former "remove from any existing list first" defensive hop is REMOVED —
// push 入口已断言 list_empty(&wait_node), 重复插入由断言抓（类 A 残余/开发期
// bug）， 不再静默吸收（与 §4.3 run_node 幂等保留不同：wait_node 无类 B
// 合法竞态， 单一归属由 insert/remove 严格配对保证）。
void sched_timer_queue_insert(int cpu, xtask *proc) {
  list_node *head = &cpu_locals[cpu].timer_queue;
  list_node *node = head->next;
  while (node != head) {
    xtask *p = LIST_ENTRY(node, xtask, wait_node);
    if (p->wait_deadline > proc->wait_deadline)
      break;
    node = node->next;
  }
  // Insert before node
  proc->wait_node.prev = node->prev;
  proc->wait_node.next = node;
  node->prev->next = &proc->wait_node;
  node->prev = &proc->wait_node;
}

// design1 §6.2: remove routes through timer_queue_wait_pop for the
// post-remove list_init (single-tenancy reset).  Kept as the canonical
// remove entry for callers that hold the lock and know the node is linked.
void sched_timer_queue_remove(xtask *proc) { timer_queue_wait_pop(proc); }

// sched_task_reap: reclaim all resources of a process
// Called by sys_exit (no-parent path) or sys_waitpid
//
// Resource release list (note SMP races):
//   immediate (no schedule() path references):
//     - iopm           : only read under sched_lock by update_tss_iopm
//     - mm (mm_put)    : refcount-managed, last releaser is responsible
//     - recv queue     : only accessed by this process
//   lazy (still referenced by schedule() switch path, deferred until slot
//   reuse in xtask_alloc):
//     - k_stack (2pg)  : switch_to asm executes ret on the child's stack
//     - fpu_page (1pg) : fpu_context_switch fxsave on this page in prev branch
//   released by BSD layer (proc_reap_hook):
//     - fds/signal/proc
//
// Freeing lazy-class resources immediately would race with schedule() and
// UAF (sched_task_reap and schedule() share no lock). Add similar resources
// to the corresponding category; lazy-class resources are uniformly released
// in xtask_alloc's reclaim_lazy_resources.
void sched_task_reap(xtask *proc) {
  ASSERT(proc->state == ZOMBIE || proc->state == REAPING);

  // 1. Free IOPM bitmap (immediate)
  if (proc->iopm) {
    kfree(proc->iopm);
    proc->iopm = NULL;
  }

  // 2b. Free FPU state page (lazy-allocated via bfc_alloc_page)
  //     Deferred until slot reuse in xtask_alloc (same rationale as the kernel
  //     stack, see the list at the top of this function): the child may still
  //     hold an fpu_page reference in schedule()'s fpu_context_switch; freeing
  //     it here would race with schedule() and UAF. By slot reuse time the
  //     child has long been switched away.
  //     if (proc->fpu_page) {
  //     bfc_free_page(proc->fpu_page, 1);
  //     proc->fpu_page = NULL;
  // }

  // 2c. Detached thread: unmap user TLS + stack BEFORE mm_put
  //     (after mm_put, pml4 may be freed by mm_release → UAF)
  if (proc->detached && proc->mm) {
    uint64_t *pml4 =
        (__force uint64_t *)phys_to_virt((__force phys_addr_t)proc->cr3);
    if (proc->tls_page && proc->tls_total) {
      size_t npages = proc->tls_total / PAGE_SIZE;
      for (size_t i = 0; i < npages; i++) {
        unmap_user_pages(pml4, proc->tls_page + i * PAGE_SIZE,
                         proc->tls_page + (i + 1) * PAGE_SIZE, 1);
      }
    }
    if (proc->user_stack_base && proc->user_stack_size) {
      size_t npages = proc->user_stack_size / PAGE_SIZE;
      for (size_t i = 0; i < npages; i++) {
        unmap_user_pages(pml4, proc->user_stack_base + i * PAGE_SIZE,
                         proc->user_stack_base + (i + 1) * PAGE_SIZE, 1);
      }
    }
  }

  // 3. mm_put (decrement triggers mm_release when ref_count hits 0)
  //    mm_release will: free user pages+PML4+mmap+SHM+devtmpfs+irq_owner+wake
  //    waiters
  if (proc->mm) {
    pid_t pid_for_cleanup = proc->pid;
    mm *mm = proc->mm;
    proc->mm = NULL;
    if (refcount_dec_and_test(&mm->m_count)) {
      mm_release(mm, pid_for_cleanup);
    }
  }

  // 4. Free any RECV_MSG / RECV_IOCTL entries in recv queue (kfree kmaddr)
  spin_lock(&proc->recv_lock);
  uint32_t idx = proc->recv_tail;
  while (idx != proc->recv_head) {
    recv_msg *m = (recv_msg *)proc->recv_buf[idx];
    if ((m->type == RECV_MSG || m->type == RECV_IOCTL) && m->msg.kmaddr) {
      kfree(m->msg.kmaddr);
      m->msg.kmaddr = NULL;
    }
    idx = (idx + 1) % RECV_QUEUE_SIZE;
  }
  spin_unlock(&proc->recv_lock);

  // 5. Clear MSG caller state (server died before responding)
  proc->msg_caller_pid = -1;

  // 6. POSIX cleanup: close fds, signal_put, free proc
  if (proc_reap_hook)
    proc_reap_hook(proc);

  // 7. Set REAPING state (dynamic: do not immediately kmem_cache_free the
  //    xtask object). Field cleanup matches the old design (pid=-1 / mm=NULL
  //    / proc freed by hook), preserving all reader guard semantics
  //    (tasks[i].pid>=0 / ==p / proc!=NULL all evaluate false on REAPING slots
  //    -> skip), the 112+ lockless reads remain safe without refcount.
  //    The xtask object itself is left for xtask_alloc to kmem_cache_free when
  //    reusing this slot (REAPING branch). Lazy resources (k_stack_top /
  //    fpu_page) are intentionally preserved, released by xtask_alloc's
  //    reclaim_lazy_resources (SMP race: schedule() may still reference them).
  spin_lock(&tasks_lock);

  // Invariant assertion: ZOMBIE should not be on any run_queue / timer_queue
  // (the state assertion is kept only at the top entry point, not duplicated
  // here — the top ASSERT already covers callers passing the wrong state, and
  // no path within this window overwrites the ZOMBIE state, so re-asserting
  // in step 7 would be redundant)
  // design1 §4.2: single-tenancy assert at destroy.  Debug build upgrades the
  // existing WARN to ASSERT (catches class-A residual + pairing bugs); release
  // build keeps WARN_ON as the prior safety net.
#ifndef NDEBUG
  ASSERT(list_empty(&proc->run_node));
#else
  WARN_ON(!list_empty(&proc->run_node));
#endif
#ifndef NDEBUG
  ASSERT(list_empty(&proc->wait_node));
#else
  WARN_ON(!list_empty(&proc->wait_node));
#endif

  proc->pid = -1;
  proc->state = REAPING; // dynamic: REAPING instead of UNUSED, xtask_alloc
                         // frees the object on reuse
  proc->k_rsp = 0;
  // proc->k_stack_top intentionally preserved (lazy reclaim)
  proc->cr3 = 0;
  proc->entry = 0;
  proc->wait_event = WAIT_NONE;
  proc->tgid = -1;
  proc->mm = NULL;
  // Note: do not clear assigned_cpu — setting it to -1 would introduce an
  // out-of-bounds race (the wake path reads it locklessly before acquiring
  // the lock)
  proc->iopm = NULL;
  proc->wait_deadline = 0;
  proc->wait_timed_out = 0;
  list_init(&proc->run_node);  // slot reuse cleanup, idempotent
  list_init(&proc->wait_node); // same as above
  // recv queue
  proc->recv_head = 0;
  proc->recv_tail = 0;
  proc->recv_lock = SPINLOCK_INIT;
  // REQ state
  proc->req_caller_pid = -1;
  proc->req_reply_buf = NULL;
  proc->req_result = 0;
  proc->req_target_pid = -1;
  proc->req_replied = 0;
  // MSG state
  proc->msg_reply_buf = NULL;
  proc->msg_reply_len = 0;
  proc->msg_caller_pid = -1;
  proc->msg_result = 0;
  proc->msg_target_pid = -1;
  proc->msg_replied = 0;
  proc->cpu_time_ns = 0;
  proc->last_sched = 0;
  // Clear threading fields
  proc->fs_base = 0;
  proc->detached = 0;
  proc->tls_page = 0;
  proc->tls_total = 0;
  proc->user_stack_base = 0;
  proc->user_stack_size = 0;
  proc->need_resched = 0;
  // proc->fpu_page intentionally preserved (lazy reclaim, see list at top of
  // function)
  spin_unlock(&tasks_lock);
}

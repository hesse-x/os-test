/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// kernel/bsd/futex.c — Futex implementation (anon key: cr3 + page_off)
// Phase 3b -> C5: FUTEX_WAIT / FUTEX_WAKE + timeout + EINTR + bucket lock
// irqsave

#include "kernel/bsd/futex.h"
#include "arch/x64/rtc.h"
#include "arch/x64/smp.h"
#include "kernel/bsd/proc.h"
#include "kernel/xcore/kpi.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mm_types.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/trap.h"
#include "kernel/xcore/xtask.h"
#include <xos/errno.h>
#include <xos/robust_list.h>
#include <xos/time.h>

struct futex_bucket futex_table[FUTEX_HASH_SIZE];

struct futex_key {
  uint32_t type; // 0=anon
  uint64_t cr3;
  uint64_t page_off;
};

static uint32_t futex_hash(struct futex_key *key) {
  uint64_t h = key->cr3 ^ key->page_off;
  return (uint32_t)((h >> 3) & (FUTEX_HASH_SIZE - 1));
}

static void get_futex_key(uint64_t uaddr, mm *mm, struct futex_key *key) {
  key->type = 0;
  key->cr3 = mm->cr3;
  key->page_off = uaddr >> 12; // PAGE_SHIFT=12
}

int64_t sys_futex(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4,
                  int64_t arg5, int64_t arg6) {
  (void)arg5;
  (void)arg6;
  uint64_t uaddr = (uint64_t)arg1;
  int op = (int)arg2;
  uint32_t val = (uint32_t)arg3;
  xtask *cur = current_task;

  // Only FUTEX_WAIT / FUTEX_WAKE are supported
  int real_op = op & 0x7f;
  if (real_op != FUTEX_WAIT && real_op != FUTEX_WAKE)
    return (int64_t)-ENOSYS;

  struct futex_key key;
  get_futex_key(uaddr, cur->mm, &key);
  struct futex_bucket *bucket = &futex_table[futex_hash(&key)];

  if (real_op == FUTEX_WAKE) {
// Collect waiters then release the bucket lock before waking: wake_with_event
// takes the target's scheduler_lock, so waking while holding the bucket lock
// would form a bucket->scheduler lock-order nesting.
//
// Note: we previously allocated xtask *to_wake[MAX_PROC] (8KB) on the stack to
// collect waiters, but the kernel stack is only 2 pages (8KB); that array
// filled the whole stack and overflowed downward, corrupting the slab object
// adjacent below the stack (typical symptom: after sys_exit's clear_tid
// futex_wake, signal->parent_pid became stack-residual garbage, and do_exit's
// access to parent->proc->sig_pending triggered #PF).
// Switched to batching: each batch uses a small fixed array to collect <= 32
// waiters and wakes them, looping until val waiters have been woken or the
// bucket has no more matching waiters. futex wake semantics is "wake at most
// val", so batching is equivalent. val comes from user space and is untrusted;
// batching also avoids the stack/heap overhead of a large val.
#define FUTEX_WAKE_BATCH 32
    int total_woken = 0;
    while (total_woken < (int)val) {
      xtask *to_wake[FUTEX_WAKE_BATCH];
      int nwake = 0;
      uint64_t bflags;
      spin_lock_irqsave(&bucket->lock, &bflags);
      list_node *node = bucket->waiters.next;
      int batch = (int)val - total_woken;
      if (batch > FUTEX_WAKE_BATCH)
        batch = FUTEX_WAKE_BATCH;
      while (node != &bucket->waiters && nwake < batch) {
        proc *p = LIST_ENTRY(node, proc, futex_node);
        node = node->next;
        if (p->futex_uaddr == uaddr) {
          to_wake[nwake++] = p->xtask;
          list_remove(&p->futex_node);
          p->futex_uaddr = 0;
        }
      }
      spin_unlock_irqrestore(&bucket->lock, bflags);
      for (int i = 0; i < nwake; i++) {
        wake_with_event(to_wake[i], WAIT_FUTEX);
      }
      total_woken += nwake;
      if (nwake < batch)
        break; // bucket has no more matching waiters
    }
    printk(LOG_DEBUG, "futex WAKE: pid=%d uaddr=%p val=%d nwake=%d\n",
           (int)cur->pid, (void *)uaddr, (int)val, total_woken);
    return (int64_t)total_woken;
#undef FUTEX_WAKE_BATCH
  }

  // FUTEX_WAIT
  // 1. Verify the value under lock (prevent lost wake-up) + enqueue
  uint64_t bflags;
  spin_lock_irqsave(&bucket->lock, &bflags);
  uint32_t cur_val;
  if (copy_from_user(&cur_val, (void __user *)uaddr, 4) != 0) {
    spin_unlock_irqrestore(&bucket->lock, bflags);
    return (int64_t)-EFAULT;
  }
  if (cur_val != val) {
    spin_unlock_irqrestore(&bucket->lock, bflags);
    printk(LOG_DEBUG, "futex WAIT EAGAIN: pid=%d uaddr=%p val=%d cur=%d\n",
           (int)cur->pid, (void *)uaddr, (int)val, (int)cur_val);
    return (int64_t)-EAGAIN;
  }
  cur->proc->futex_uaddr = uaddr;
  list_push_back(&bucket->waiters, &cur->proc->futex_node);
  printk(LOG_DEBUG, "futex WAIT: pid=%d uaddr=%p val=%d\n", (int)cur->pid,
         (void *)uaddr, (int)val);

  // timeout: arg4 = absolute abstime ns (struct timespec * passed as int64_t)
  int64_t abstime_ns = 0;
  int has_timeout = 0;
  if (arg4 != 0) {
    struct timespec ts;
    if (copy_from_user(&ts, (void __user *)arg4, sizeof(ts)) != 0) {
      list_remove(&cur->proc->futex_node);
      cur->proc->futex_uaddr = 0;
      spin_unlock_irqrestore(&bucket->lock, bflags);
      return (int64_t)-EFAULT;
    }
    abstime_ns = (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
    has_timeout = 1;
  }

  // 2. Set BLOCKED (hold bucket lock to close the lost-wakeup window)
  int cpu = cur->assigned_cpu;
  uint64_t flags;
  spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &flags);
  cur->wait_timed_out = 0;
  if (has_timeout) {
    // The abstime passed by pthread_mutex_timedlock/etc. is a CLOCK_REALTIME
    // absolute instant (POSIX).  The timer_queue compares wait_deadline against
    // sched_clock() (monotonic, zero at boot), so convert the wall-clock
    // abstime into sched_clock coordinates by subtracting the boot wall-clock
    // baseline.  Before S14, CLOCK_REALTIME == sched_clock() so this was a
    // no-op; now CLOCK_REALTIME = wall_clock_boot_ns + sched_clock() and the
    // subtraction is required.  Underflow (past time) wraps to a huge deadline
    // that still lands on the right value modulo 2^64.
    uint64_t boot_ns = __atomic_load_n(&wall_clock_boot_ns, __ATOMIC_RELAXED);
    cur->wait_deadline = (uint64_t)abstime_ns - boot_ns;
    sched_timer_queue_insert(cpu, cur);
  }
  cur->wait_event = WAIT_FUTEX;
  cur->state = BLOCKED;
  // Re-check signal_pending AFTER arming the wait. A signal (e.g. SIGCANCEL
  // from pthread_cancel) may have pended between the value check above
  // (line ~116) and here — while cur was still RUNNING, so wake_process_any
  // in deliver_signal_to was a no-op (it only wakes BLOCKED targets). Without
  // this recheck, cur would block in schedule() with a pending signal and
  // never be woken (the signal already fired its wake attempt, to no avail
  // because the target was not yet BLOCKED) → lost wake-up / permanent hang
  // (the test_pthread_join_cancel deadlock, see bug.md). Mirror Linux's
  // interruptible-wait pattern: set TASK_INTERRUPTIBLE, then re-check
  // signal_pending before schedule(). Abort the wait and let the normal
  // syscall return path (xcall_dispatch → check_pending_signals) deliver the
  // pending signal.
  if (signal_pending_hook && signal_pending_hook(cur)) {
    cur->state = RUNNING;
    cur->wait_event = WAIT_NONE;
    if (has_timeout)
      sched_timer_queue_cancel(cur);
    spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, flags);
    // bucket->lock is still held from the value-check above (line ~116);
    // remove from the waiter list directly. Re-acquiring it here would be a
    // recursive same-CPU lock and trip spinlock.h's BUG_ON (the futex-wait
    // signal-recheck deadlock).
    if (cur->proc->futex_uaddr) {
      list_remove(&cur->proc->futex_node);
      cur->proc->futex_uaddr = 0;
    }
    spin_unlock_irqrestore(&bucket->lock, bflags);
    // Same return value as the post-wakeup path: untimed→-ERESTART,
    // timed→-EINTR. xcall_dispatch then runs check_pending_signals, which
    // delivers the pending signal (e.g. SIGCANCEL → __pthread_cancel_check →
    // pthread_exit).
    return has_timeout ? (int64_t)-EINTR : (int64_t)-ERESTART;
  }
  spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, flags);
  spin_unlock_irqrestore(&bucket->lock, bflags);
  schedule();

  // 3. Post-wakeup cleanup + return value decision
  int64_t ret_val = 0;
  if (signal_pending_hook && signal_pending_hook(cur))
    // 02: a timed FUTEX_WAIT surfaces -EINTR (restarting would lose the elapsed
    // time); an untimed wait returns -ERESTART so SA_RESTART re-executes it.
    ret_val = has_timeout ? (int64_t)-EINTR : (int64_t)-ERESTART;
  else if (cur->wait_timed_out)
    ret_val = (int64_t)-ETIMEDOUT;

  spin_lock_irqsave(&bucket->lock, &bflags);
  if (cur->proc->futex_uaddr) {
    list_remove(&cur->proc->futex_node);
    cur->proc->futex_uaddr = 0;
  }
  spin_unlock_irqrestore(&bucket->lock, bflags);
  return ret_val;
}

// ===================== Robust-futex list =====================
// musl pthread robust mutexes register a per-thread linked list of held locks
// via set_robust_list(2). On thread exit the kernel walks it: for any futex
// still owned by the dying thread, set FUTEX_OWNER_DIED and wake one waiter so
// a blocked acquirer can detect the dead owner and recover the lock. Mirrors
// Linux's exit_robust_list / handle_futex_death.
//
// Runs from do_exit_with_code step 4, before ZOMBIE (proc->proc alive) and
// before mm_put (pml4 alive so copy_from_user on the dying thread's user
// pointers works).

static void robust_mark_died(uintptr_t uaddr, pid_t owner_tid) {
  uint32_t uval;
  if (copy_from_user(&uval, (void __user *)uaddr, sizeof(uval)) != 0)
    return; // bad futex word — skip this entry
  // Only mark locks actually owned by the dying thread. A lock held by another
  // thread (or unowned) is left untouched.
  if ((uval & FUTEX_TID_MASK) != (uint32_t)owner_tid)
    return;
  uint32_t nval = (uval & FUTEX_TID_MASK) | FUTEX_OWNER_DIED;
  (void)copy_to_user((void __user *)uaddr, &nval, sizeof(nval));
  // Wake one waiter the same way clear_tid_addr does on thread exit.
  sys_futex((int64_t)uaddr, (int64_t)FUTEX_WAKE, 1, 0, 0, 0);
}

void exit_robust_list(struct proc *bp, pid_t owner_tid) {
  if (!bp || !bp->robust_list_head)
    return; // not registered — no-op for the in-tree pthread
  struct robust_list_head head;
  if (copy_from_user(&head, (void __user *)bp->robust_list_head,
                     sizeof(head)) != 0)
    return; // bad head pointer — cannot walk safely, give up
  long foff = head.futex_offset;

  // A lock being acquired/released at the instant of exit never made it onto
  // (or off) the .next chain; handle it first so its waiter is released.
  if ((uintptr_t)head.list_op_pending)
    robust_mark_died((uintptr_t)head.list_op_pending + foff, owner_tid);

  // Walk the chain of held locks. Bound the walk so a cyclic/hostile list
  // cannot keep the kernel here forever.
  struct robust_list *entry = head.list.next;
  for (int i = 0; entry && i < ROBUST_LIST_LIMIT; i++) {
    struct robust_list node;
    if (copy_from_user(&node, (void __user *)entry, sizeof(node)) != 0)
      break; // bad node — list untrustworthy, stop (matches Linux)
    robust_mark_died((uintptr_t)entry + foff, owner_tid);
    entry = node.next;
  }
}

int64_t sys_set_robust_list(int64_t head, int64_t len, int64_t unused1,
                            int64_t unused2, int64_t unused3, int64_t unused4) {
  (void)unused1;
  (void)unused2;
  (void)unused3;
  (void)unused4;
  // Linux requires len == sizeof(struct robust_list_head); a mismatched len
  // signals an ABI mismatch, so reject rather than store a bogus size. head
  // may be NULL (deregistration).
  if ((size_t)len != sizeof(struct robust_list_head))
    return (int64_t)-EINVAL;
  current_task->proc->robust_list_head = (void *)(uintptr_t)head;
  current_task->proc->robust_list_len = (size_t)len;
  return 0;
}

int64_t sys_get_robust_list(int64_t pid, int64_t head_ptr, int64_t len_ptr,
                            int64_t unused1, int64_t unused2, int64_t unused3) {
  (void)unused1;
  (void)unused2;
  (void)unused3;
  // Reading another task's robust list requires CAP_SYS_PTRACE in Linux; this
  // kernel has no ptrace/capability gate, so restrict to self (pid==0 or
  // current->pid).
  if ((pid_t)pid != 0 && (pid_t)pid != current_task->pid)
    return (int64_t)-EPERM;
  void *head = current_task->proc->robust_list_head;
  size_t len = current_task->proc->robust_list_len;
  if (head_ptr &&
      copy_to_user((void __user *)head_ptr, &head, sizeof(head)) != 0)
    return (int64_t)-EFAULT;
  if (len_ptr && copy_to_user((void __user *)len_ptr, &len, sizeof(len)) != 0)
    return (int64_t)-EFAULT;
  return 0;
}

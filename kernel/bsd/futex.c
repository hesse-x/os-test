/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// kernel/bsd/futex.c — Futex implementation (anon key: cr3 + page_off)
// Phase 3b -> C5: FUTEX_WAIT / FUTEX_WAKE + timeout + EINTR + bucket lock
// irqsave

#include "kernel/bsd/futex.h"

#include <stdbool.h>

#include "arch/x64/apic.h"
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
  int real_op = op & 0x7f; // FUTEX_PRIVATE (128) is eaten by the 0x7f mask

  // Batched wake collector cap: under the bucket lock only collect into a small
  // fixed array, then wake_with_event after releasing the bucket lock (it takes
  // the target scheduler_lock; waking under the bucket lock would form a
  // bucket→scheduler lock-order nesting). The kernel stack is only 2 pages
  // (8KB); an earlier xtask*[MAX_PROC] (8KB) overflowed the stack. REQUEUE and
  // WAKE share this batched pattern. Function-scope macro: both WAKE and
  // REQUEUE branches reference it.
#define FUTEX_WAKE_BATCH 32

  // FUTEX_REQUEUE / FUTEX_CMP_REQUEUE — musl pthread condvar hand-off
  // (pthread_cond_timedwait.c unlock_requeue): moves the next waiter blocked on
  // the barrier from cv to mutex, saving a user-space round-trip and avoiding a
  // thundering herd. musl calls it as
  //   REQUEUE(uaddr1=cv, nr_wake=0, nr_requeue=1, *, uaddr2=mutex)
  // — first a_store(l,0) releases the cv lock, then requeue without waking
  // (nr_wake=0): the migrated waiter keeps sleeping on mutex, woken by a future
  // mutex unlock WAKE. This repo's waiters only record futex_uaddr (no expected
  // value), so matching is purely futex_uaddr==uaddr1 (same as the WAKE pattern
  // at futex.c:94). CMP_REQUEUE's val2 is used by Linux for a second-address
  // value check, which this repo lacks; musl passes val2=0, so it's harmless.
  if (real_op == FUTEX_REQUEUE || real_op == FUTEX_CMP_REQUEUE) {
    uint64_t uaddr1 = uaddr;
    int nr_wake = (int)val; // arg3
    int nr_requeue = (int)arg4;
    uint64_t cmpval = (real_op == FUTEX_CMP_REQUEUE) ? (uint64_t)arg5 : 0;
    uint64_t uaddr2 =
        (real_op == FUTEX_REQUEUE) ? (uint64_t)arg5 : (uint64_t)arg6;
    // CMP check: *uaddr1 != cmpval → -EAGAIN (pre-delivery atomicity
    // guarantee, same as Linux). copy_from_user handles user-pointer
    // out-of-range/unreadable → EFAULT, matching the WAIT path (futex.c:119);
    // this repo's futex paths do no explicit uaddr bounds check.
    if (cmpval) {
      uint32_t cur_val1;
      if (copy_from_user(&cur_val1, (void __user *)uaddr1, 4) != 0)
        return (int64_t)-EFAULT;
      if (cur_val1 != (uint32_t)cmpval)
        return (int64_t)-EAGAIN;
    }

    struct futex_key key1, key2;
    get_futex_key(uaddr1, cur->mm, &key1);
    get_futex_key(uaddr2, cur->mm, &key2);
    struct futex_bucket *bucket1 = &futex_table[futex_hash(&key1)];
    struct futex_bucket *bucket2 = &futex_table[futex_hash(&key2)];
    uint32_t h1 = futex_hash(&key1), h2 = futex_hash(&key2);

    // Dual bucket lock order (deadlock avoidance): lock by hash value low→high,
    // globally consistent. Same-bucket special case locks only once (spinlock.h
    // forbids same-CPU re-entry). The wake part reuses WAKE's batched-collect +
    // wake_with_event-after-bucket-unlock pattern (futex.c:65-67): avoids
    // taking the target scheduler_lock while holding the bucket lock, which
    // would form a bucket→scheduler lock-order nesting.
    int woken = 0, requeued = 0;
    while (woken < nr_wake || requeued < nr_requeue) {
      xtask *to_wake[FUTEX_WAKE_BATCH];
      int nwake = 0;
      bool same_bucket = (bucket1 == bucket2);
      uint64_t b1f = 0, b2f = 0;
      if (same_bucket) {
        spin_lock_irqsave(&bucket1->lock, &b1f);
      } else if (h1 < h2) {
        spin_lock_irqsave(&bucket1->lock, &b1f);
        spin_lock_irqsave(&bucket2->lock, &b2f);
      } else {
        spin_lock_irqsave(&bucket2->lock, &b2f);
        spin_lock_irqsave(&bucket1->lock, &b1f);
      }

      // Walk bucket1, matching futex_uaddr==uaddr1 (same as WAKE): fill nr_wake
      // into the wake batch; the rest get futex_uaddr=uaddr2 and move to
      // bucket2 (no wake). Requeued waiters stay state=BLOCKED,
      // wait_event=WAIT_FUTEX, only the wait address changes → a future mutex
      // WAKE matching futex_uaddr==mutex naturally hits them (futex.c:94).
      list_node *node = bucket1->waiters.next;
      int batch_wake = nr_wake - woken;
      if (batch_wake > FUTEX_WAKE_BATCH)
        batch_wake = FUTEX_WAKE_BATCH;
      while (node != &bucket1->waiters) {
        if (woken >= nr_wake && requeued >= nr_requeue)
          break;
        proc *p = LIST_ENTRY(node, proc, futex_node);
        list_node *next = node->next;
        node = next;
        if (p->futex_uaddr != uaddr1)
          continue;
        if (woken < nr_wake && nwake < batch_wake) {
          to_wake[nwake++] = p->xtask;
          list_remove(&p->futex_node);
          p->futex_uaddr = 0;
          woken++;
        } else if (requeued < nr_requeue) {
          list_remove(&p->futex_node);
          p->futex_uaddr = uaddr2;
          list_push_back(&bucket2->waiters, &p->futex_node);
          requeued++;
        }
      }

      if (same_bucket) {
        spin_unlock_irqrestore(&bucket1->lock, b1f);
      } else {
        spin_unlock_irqrestore(&bucket1->lock, b1f);
        spin_unlock_irqrestore(&bucket2->lock, b2f);
      }
      for (int i = 0; i < nwake; i++)
        wake_with_event(to_wake[i], WAIT_FUTEX);

      // Batch not full → bucket1 has no more matching waiters; finish
      // (equivalent to WAKE's futex.c:105-106).
      if (nwake < batch_wake && !(requeued < nr_requeue && requeued > 0)) {
        // Note: when nr_wake==0 (musl's actual call) batch_wake==0 and nwake is
        // always 0; this branch only decides by whether requeued advanced. If
        // requeued didn't change, bucket1 has no matching waiter to migrate →
        // stop.
        if (requeued == 0 || requeued >= nr_requeue)
          break;
      }
      if (nwake == 0 && requeued == 0)
        break;
    }
    printk(LOG_DEBUG,
           "futex REQUEUE: pid=%d u1=%p u2=%p woken=%d requeued=%d\n",
           (int)cur->pid, (void *)uaddr1, (void *)uaddr2, woken, requeued);
    return (int64_t)(woken + requeued);
  }

  // Only FUTEX_WAIT / FUTEX_WAKE are supported beyond this point
  if (real_op != FUTEX_WAIT && real_op != FUTEX_WAKE)
    return (int64_t)-ENOSYS;

  struct futex_key key;
  get_futex_key(uaddr, cur->mm, &key);
  struct futex_bucket *bucket = &futex_table[futex_hash(&key)];

  if (real_op == FUTEX_WAKE) {
    // Collect waiters then release the bucket lock before waking:
    // wake_with_event takes the target's scheduler_lock, so waking while
    // holding the bucket lock would form a bucket->scheduler lock-order
    // nesting.
    //
    // Note: we previously allocated xtask *to_wake[MAX_PROC] (8KB) on the stack
    // to collect waiters, but the kernel stack is only 2 pages (8KB); that
    // array filled the whole stack and overflowed downward, corrupting the slab
    // object adjacent below the stack (typical symptom: after sys_exit's
    // clear_tid futex_wake, signal->parent_pid became stack-residual garbage,
    // and do_exit's access to parent->proc->sig_pending triggered #PF).
    // Switched to batching: each batch uses a small fixed array to collect <=
    // 32 waiters and wakes them, looping until val waiters have been woken or
    // the bucket has no more matching waiters. futex wake semantics is "wake at
    // most val", so batching is equivalent. val comes from user space and is
    // untrusted; batching also avoids the stack/heap overhead of a large val.
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

  // FUTEX_WAIT uses a relative timeout. Absolute deadlines belong to
  // FUTEX_WAIT_BITSET, which is not implemented here.
  uint64_t timeout_ns = 0;
  int has_timeout = 0;
  if (arg4 != 0) {
    struct timespec ts;
    if (copy_from_user(&ts, (void __user *)arg4, sizeof(ts)) != 0) {
      list_remove(&cur->proc->futex_node);
      cur->proc->futex_uaddr = 0;
      spin_unlock_irqrestore(&bucket->lock, bflags);
      return (int64_t)-EFAULT;
    }
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L) {
      list_remove(&cur->proc->futex_node);
      cur->proc->futex_uaddr = 0;
      spin_unlock_irqrestore(&bucket->lock, bflags);
      return (int64_t)-EINVAL;
    }
    timeout_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    has_timeout = 1;
  }

  // 2. Set BLOCKED (hold bucket lock to close the lost-wakeup window)
  int cpu = cur->assigned_cpu;
  uint64_t flags;
  spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &flags);
  cur->wait_timed_out = 0;
  if (has_timeout) {
    cur->wait_deadline = sched_clock() + timeout_ns;
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

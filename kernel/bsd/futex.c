/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// kernel/bsd/futex.c — Futex implementation (anon key: cr3 + page_off)
// Phase 3b -> C5: FUTEX_WAIT / FUTEX_WAKE + timeout + EINTR + bucket lock
// irqsave

#include "kernel/bsd/futex.h"
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
    cur->wait_deadline = abstime_ns; // absolute abstime
    sched_timer_queue_insert(cpu, cur);
  }
  cur->wait_event = WAIT_FUTEX;
  cur->state = BLOCKED;
  spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, flags);
  spin_unlock_irqrestore(&bucket->lock, bflags);
  schedule();

  // 3. Post-wakeup cleanup + return value decision
  int64_t ret_val = 0;
  if (signal_pending_hook && signal_pending_hook(cur))
    ret_val = (int64_t)-EINTR;
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

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/xcore/completion.h"

#include <limits.h>
#include <stdbool.h>
#include <xos/errno.h>

#include "arch/x64/apic.h"
#include "arch/x64/smp.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/xtask.h"

static void completion_wake(wait_queue_t *wait, unsigned long flags) {
  (void)flags;
  wake_wq_target((xtask *)wait->data);
}

void init_completion(completion *c) {
  c->lock = SPINLOCK_INIT;
  c->done = 0;
  init_wait_queue_head(&c->waiters);
}

void reinit_completion(completion *c) {
  uint64_t flags;
  spin_lock_irqsave(&c->lock, &flags);
  c->done = 0;
  spin_unlock_irqrestore(&c->lock, flags);
}

static void completion_publish(completion *c, bool all) {
  uint64_t flags;
  spin_lock_irqsave(&c->lock, &flags);
  if (all)
    c->done = UINT_MAX;
  else if (c->done != UINT_MAX)
    c->done++;
  spin_unlock_irqrestore(&c->lock, flags);
  __wake_up(&c->waiters, 0);
}

void complete(completion *c) { completion_publish(c, false); }

void complete_all(completion *c) { completion_publish(c, true); }

int wait_for_completion_timeout(completion *c, uint64_t timeout_ns) {
  if (!current_task)
    return -EINVAL;

  wait_queue_t wait = {
      .func = completion_wake, .data = current_task, .exclusive = 0};
  list_init(&wait.node);
  add_wait_queue(&c->waiters, &wait);

  uint64_t now = sched_clock();
  uint64_t deadline = 0;
  if (timeout_ns != UINT64_MAX) {
    deadline = UINT64_MAX - now < timeout_ns ? UINT64_MAX : now + timeout_ns;
  }

  int result = 0;
  for (;;) {
    uint64_t cflags;
    spin_lock_irqsave(&c->lock, &cflags);
    if (c->done) {
      if (c->done != UINT_MAX)
        c->done--;
      spin_unlock_irqrestore(&c->lock, cflags);
      break;
    }
    if (signal_pending(current_task)) {
      spin_unlock_irqrestore(&c->lock, cflags);
      result = -EINTR;
      break;
    }
    if (deadline && sched_clock() >= deadline) {
      spin_unlock_irqrestore(&c->lock, cflags);
      result = -ETIMEDOUT;
      break;
    }

    int cpu = current_task->assigned_cpu;
    uint64_t sflags;
    spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &sflags);
    current_task->state = BLOCKED;
    current_task->wait_event = WAIT_COMPLETION;
    current_task->wait_timed_out = 0;
    if (deadline) {
      current_task->wait_deadline = deadline;
      timer_queue_wait_push(cpu, current_task);
    }
    spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, sflags);
    spin_unlock_irqrestore(&c->lock, cflags);
    schedule();
  }

  remove_wait_queue(&c->waiters, &wait);
  return result;
}

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "arch/x64/smp.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/mutex.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/xtask.h"

static void mutex_wake(wait_queue_t *wait, unsigned long flags) {
  (void)flags;
  wake_wq_target((xtask *)wait->data);
}

void mutex_init(mutex *m) {
  m->guard = SPINLOCK_INIT;
  m->locked = 0;
  init_wait_queue_head(&m->waiters);
}

void mutex_lock(mutex *m) {
  // Early boot is single-threaded and cannot call schedule().
  if (!current_task) {
    for (;;) {
      spin_lock(&m->guard);
      if (!m->locked) {
        m->locked = 1;
        spin_unlock(&m->guard);
        return;
      }
      spin_unlock(&m->guard);
      __asm__ volatile("pause");
    }
  }

  wait_queue_t wait = {
      .func = mutex_wake, .data = current_task, .exclusive = 1};
  list_init(&wait.node);
  add_wait_queue(&m->waiters, &wait);

  for (;;) {
    uint64_t flags;
    spin_lock_irqsave(&m->guard, &flags);
    if (!m->locked) {
      m->locked = 1;
      spin_unlock_irqrestore(&m->guard, flags);
      remove_wait_queue(&m->waiters, &wait);
      return;
    }

    int cpu = current_task->assigned_cpu;
    spin_lock(&cpu_locals[cpu].scheduler_lock);
    current_task->state = BLOCKED;
    current_task->wait_event = WAIT_MUTEX;
    spin_unlock(&cpu_locals[cpu].scheduler_lock);
    spin_unlock_irqrestore(&m->guard, flags);
    schedule();
  }
}

void mutex_unlock(mutex *m) {
  uint64_t flags;
  spin_lock_irqsave(&m->guard, &flags);
  m->locked = 0;
  spin_unlock_irqrestore(&m->guard, flags);
  __wake_up(&m->waiters, 0);
}

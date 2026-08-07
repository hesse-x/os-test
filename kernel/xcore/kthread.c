/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/xcore/kthread.h"

#include <xos/errno.h>

#include "arch/x64/apic.h"
#include "arch/x64/memlayout.h"
#include "arch/x64/paging.h"
#include "arch/x64/smp.h"
#include "arch/x64/utils.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/completion.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/wait_queue.h"
#include "kernel/xcore/xtask.h"

struct kthread {
  xtask *task;
  int (*fn)(void *);
  void *arg;
  char name[32];
  atomic_t stop;
  atomic_t terminated;
  bool detached;
  spinlock sleep_lock;
  bool wake_pending;
  wait_queue_head sleep_wq;
  completion exited;
  int result;
};

struct kthread_switch_frame {
  uint64_t rbx, rbp, r12, r13, r14, r15, ret_addr;
};

static void kthread_wait_wake(wait_queue_t *wait, unsigned long flags) {
  (void)flags;
  wake_wq_target((xtask *)wait->data);
}

static void kthread_entry(void) {
  struct kthread *thread = (struct kthread *)current_task->kernel_private;
  sti();
  thread->result = thread->fn(thread->arg);

  /* Publish completion with IRQs disabled so join cannot observe completion
   * before the task has made itself reapable. */
  cli();
  if (thread->detached) {
    current_task->state = ZOMBIE;
    sched_task_reap(current_task);
    kfree(thread);
    schedule();
    panic("detached kthread resumed after exit");
  }
  complete(&thread->exited);
  current_task->state = ZOMBIE;
  atomic_set(&thread->terminated, 1);
  schedule();
  panic("kthread %s resumed after exit", thread->name);
}

static uint64_t kthread_build_stack(uint64_t top) {
  struct kthread_switch_frame frame = {0};
  frame.ret_addr = (uint64_t)kthread_entry;
  // switch_to's final ret must enter kthread_entry with RSP % 16 == 8, just
  // like a normal SysV call. Leave one word above the synthetic frame.
  uint8_t *sp = (uint8_t *)top - sizeof(frame) - sizeof(uint64_t);
  __memcpy(sp, &frame, sizeof(frame));
  return (uint64_t)sp;
}

static struct kthread *kthread_create_common(int (*fn)(void *), void *arg,
                                             const char *name, bool detached) {
  if (!fn)
    return NULL;
  struct kthread *thread = kmalloc(sizeof(*thread));
  if (!thread)
    return NULL;
  __memset(thread, 0, sizeof(*thread));
  thread->fn = fn;
  thread->arg = arg;
  thread->detached = detached;
  thread->sleep_lock = SPINLOCK_INIT;
  init_wait_queue_head(&thread->sleep_wq);
  init_completion(&thread->exited);
  atomic_set(&thread->stop, 0);
  atomic_set(&thread->terminated, 0);
  if (name) {
    int i = 0;
    for (; name[i] && i < (int)sizeof(thread->name) - 1; i++)
      thread->name[i] = name[i];
    thread->name[i] = '\0';
  }

  spin_lock(&tasks_lock);
  pid_t pid = -1;
  xtask *task = xtask_alloc(&pid);
  if (!task) {
    spin_unlock(&tasks_lock);
    kfree(thread);
    return NULL;
  }
  struct page *stack = bfc_alloc_page(KERNEL_STACK_PAGES);
  if (!stack) {
    task->pid = pid;
    task->state = REAPING;
    task->exit_done = 1;
    spin_unlock(&tasks_lock);
    kfree(thread);
    return NULL;
  }
  uint64_t top =
      (__force uint64_t)phys_to_virt(page_to_phys(stack)) + KERNEL_STACK_SIZE;
  int cpu = sched_pick_cpu();
  task->assigned_cpu = cpu;
  task->pid = pid;
  task->tgid = pid;
  task->state = READY;
  task->k_stack_top = top;
  task->k_rsp = kthread_build_stack(top);
  task->cr3 = (__force uint64_t)PHY_ADDR((uintptr_t)pml4);
  task->proc = NULL;
  task->mm = NULL;
  task->kernel_private = thread;
  kstack_canary_write(task);
  thread->task = task;
  spin_unlock(&tasks_lock);

  uint64_t flags;
  spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &flags);
  run_queue_push(cpu, task);
  spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, flags);
  return thread;
}

struct kthread *kthread_create(int (*fn)(void *), void *arg, const char *name) {
  return kthread_create_common(fn, arg, name, false);
}

int kthread_run_detached(int (*fn)(void *), void *arg, const char *name) {
  struct kthread *thread = kthread_create_common(fn, arg, name, true);
  if (!thread)
    return -ENOMEM;
  return 0;
}

bool kthread_should_stop(struct kthread *thread) {
  return thread && atomic_read(&thread->stop) != 0;
}

struct xtask *kthread_task(struct kthread *thread) {
  return thread ? thread->task : NULL;
}

void kthread_wake(struct kthread *thread) {
  if (!thread)
    return;
  uint64_t flags;
  spin_lock_irqsave(&thread->sleep_lock, &flags);
  thread->wake_pending = true;
  spin_unlock_irqrestore(&thread->sleep_lock, flags);
  __wake_up(&thread->sleep_wq, 0);
}

int kthread_wait_timeout(struct kthread *thread, uint64_t deadline_ns) {
  if (!thread || current_task != thread->task)
    return -EINVAL;
  wait_queue_t wait = {
      .func = kthread_wait_wake, .data = current_task, .exclusive = 1};
  list_init(&wait.node);
  add_wait_queue(&thread->sleep_wq, &wait);

  int result = 0;
  uint64_t flags;
  spin_lock_irqsave(&thread->sleep_lock, &flags);
  if (thread->wake_pending || kthread_should_stop(thread)) {
    thread->wake_pending = false;
    spin_unlock_irqrestore(&thread->sleep_lock, flags);
    remove_wait_queue(&thread->sleep_wq, &wait);
    return 0;
  }
  if (deadline_ns && sched_clock() >= deadline_ns) {
    spin_unlock_irqrestore(&thread->sleep_lock, flags);
    remove_wait_queue(&thread->sleep_wq, &wait);
    return -ETIMEDOUT;
  }

  int cpu = current_task->assigned_cpu;
  uint64_t sflags;
  spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &sflags);
  current_task->state = BLOCKED;
  current_task->wait_event = WAIT_KTHREAD;
  current_task->wait_timed_out = 0;
  if (deadline_ns) {
    current_task->wait_deadline = deadline_ns;
    timer_queue_wait_push(cpu, current_task);
  }
  spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, sflags);
  spin_unlock_irqrestore(&thread->sleep_lock, flags);
  schedule();

  spin_lock_irqsave(&thread->sleep_lock, &flags);
  if (thread->wake_pending) {
    thread->wake_pending = false;
  } else if (deadline_ns && sched_clock() >= deadline_ns) {
    result = -ETIMEDOUT;
  }
  spin_unlock_irqrestore(&thread->sleep_lock, flags);
  remove_wait_queue(&thread->sleep_wq, &wait);
  return result;
}

int kthread_stop(struct kthread *thread) {
  if (!thread)
    return -EINVAL;
  ASSERT(current_task != thread->task);
  atomic_set(&thread->stop, 1);
  kthread_wake(thread);
  int rc = wait_for_completion_timeout(&thread->exited, UINT64_MAX);
  if (rc)
    return rc;
  while (!atomic_read(&thread->terminated))
    schedule();
  int result = thread->result;
  sched_task_reap(thread->task);
  kfree(thread);
  return result;
}

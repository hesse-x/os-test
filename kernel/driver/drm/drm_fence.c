/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/driver/drm/drm_fence.h"

#include <stddef.h>

#include "arch/x64/apic.h"
#include "arch/x64/smp.h"
#include "arch/x64/utils.h"
#include "kernel/driver/drm/drm_core.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/xtask.h"

#include <xos/errno.h>

int bsd_sync_file_fd_install(struct xtask *proc, struct drm_fence *fence);

static void drm_fence_wake(wait_queue_t *wait, unsigned long flags) {
  (void)flags;
  wake_wq_target((xtask *)wait->data);
}

struct drm_fence *drm_fence_create(bool signaled) {
  struct drm_fence *fence = kmalloc(sizeof(*fence));
  if (!fence)
    return NULL;
  __memset(fence, 0, sizeof(*fence));
  refcount_set(&fence->refcount, 1);
  fence->lock = (spinlock)SPINLOCK_INIT;
  fence->signaled = signaled;
  init_wait_queue_head(&fence->wq);
  return fence;
}

void drm_fence_get(struct drm_fence *fence) {
  if (fence)
    refcount_inc(&fence->refcount);
}

static void drm_fence_drop_objects(struct drm_gem_object **objects,
                                   uint32_t count) {
  for (uint32_t i = 0; i < count; i++)
    drm_gem_object_put(objects[i]);
  kfree(objects);
}

void drm_fence_put(struct drm_fence *fence) {
  if (!fence || !refcount_dec_and_test(&fence->refcount))
    return;
  drm_fence_drop_objects(fence->objects, fence->object_count);
  kfree(fence);
}

bool drm_fence_is_signaled(struct drm_fence *fence) {
  if (!fence)
    return false;
  uint64_t flags;
  spin_lock_irqsave(&fence->lock, &flags);
  bool signaled = fence->signaled;
  spin_unlock_irqrestore(&fence->lock, flags);
  return signaled;
}

void drm_fence_signal(struct drm_fence *fence) {
  if (!fence)
    return;
  uint64_t flags;
  spin_lock_irqsave(&fence->lock, &flags);
  if (fence->signaled) {
    spin_unlock_irqrestore(&fence->lock, flags);
    return;
  }
  fence->signaled = true;
  struct drm_gem_object **objects = fence->objects;
  uint32_t count = fence->object_count;
  fence->objects = NULL;
  fence->object_count = 0;
  spin_unlock_irqrestore(&fence->lock, flags);
  drm_fence_drop_objects(objects, count);
  __wake_up(&fence->wq, 0);
}

int drm_fence_wait(struct drm_fence *fence, uint64_t timeout_ns) {
  if (!fence || !current_task)
    return -EINVAL;
  wait_queue_t wait = {
      .func = drm_fence_wake, .data = current_task, .exclusive = 0};
  list_init(&wait.node);
  add_wait_queue(&fence->wq, &wait);

  uint64_t now = sched_clock();
  uint64_t deadline = 0;
  bool finite = timeout_ns != UINT64_MAX;
  if (finite)
    deadline = UINT64_MAX - now < timeout_ns ? UINT64_MAX : now + timeout_ns;

  int result = 0;
  for (;;) {
    uint64_t fence_flags;
    spin_lock_irqsave(&fence->lock, &fence_flags);
    if (fence->signaled) {
      spin_unlock_irqrestore(&fence->lock, fence_flags);
      break;
    }
    if (signal_pending(current_task)) {
      spin_unlock_irqrestore(&fence->lock, fence_flags);
      result = -EINTR;
      break;
    }
    if (finite && sched_clock() >= deadline) {
      spin_unlock_irqrestore(&fence->lock, fence_flags);
      result = -ETIMEDOUT;
      break;
    }

    int cpu = current_task->assigned_cpu;
    uint64_t sched_flags;
    spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &sched_flags);
    current_task->state = BLOCKED;
    current_task->wait_event = WAIT_COMPLETION;
    current_task->wait_timed_out = 0;
    if (finite) {
      current_task->wait_deadline = deadline;
      timer_queue_wait_push(cpu, current_task);
    }
    spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, sched_flags);
    spin_unlock_irqrestore(&fence->lock, fence_flags);
    schedule();
  }
  remove_wait_queue(&fence->wq, &wait);
  return result;
}

int drm_fence_install_sync_file(struct drm_fence *fence, struct xtask *proc) {
  if (!fence || !proc)
    return -EINVAL;
  drm_fence_get(fence);
  int fd = bsd_sync_file_fd_install(proc, fence);
  if (fd < 0)
    drm_fence_put(fence);
  return fd;
}

int drm_fence_hold_objects(struct drm_fence *fence,
                           struct drm_gem_object **objects, uint32_t count) {
  if (!fence || (count && !objects))
    return -EINVAL;
  uint64_t flags;
  spin_lock_irqsave(&fence->lock, &flags);
  if (fence->signaled || fence->objects) {
    spin_unlock_irqrestore(&fence->lock, flags);
    return -EINVAL;
  }
  fence->objects = objects;
  fence->object_count = count;
  spin_unlock_irqrestore(&fence->lock, flags);
  return 0;
}

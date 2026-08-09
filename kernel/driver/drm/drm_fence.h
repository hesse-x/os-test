/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_DRIVER_DRM_FENCE_H
#define KERNEL_DRIVER_DRM_FENCE_H

#include <stdbool.h>
#include <stdint.h>

#include "kernel/xcore/atomic.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/wait_queue.h"

struct drm_gem_object;
struct xtask;

struct drm_fence {
  refcount_t refcount;
  spinlock lock;
  wait_queue_head wq;
  bool signaled;
  struct drm_gem_object **objects;
  uint32_t object_count;
};

struct drm_fence *drm_fence_create(bool signaled);
void drm_fence_get(struct drm_fence *fence);
void drm_fence_put(struct drm_fence *fence);
bool drm_fence_is_signaled(struct drm_fence *fence);
void drm_fence_signal(struct drm_fence *fence);
int drm_fence_wait(struct drm_fence *fence, uint64_t timeout_ns);
int drm_fence_install_sync_file(struct drm_fence *fence, struct xtask *proc);
int drm_fence_hold_objects(struct drm_fence *fence,
                           struct drm_gem_object **objects, uint32_t count);

#endif

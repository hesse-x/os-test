/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>

#include "kernel/driver/dma_resv.h"

#include <xos/errno.h>

#include "kernel/driver/drm/drm_fence.h"

void dma_resv_init(struct dma_resv *resv) {
  resv->lock = SPINLOCK_INIT;
  resv->exclusive = NULL;
  resv->shared_count = 0;
  resv->generation = 0;
}

void dma_resv_fini(struct dma_resv *resv) {
  drm_fence_put(resv->exclusive);
  for (uint32_t i = 0; i < resv->shared_count; i++)
    drm_fence_put(resv->shared[i]);
}

static void dma_resv_prune_locked(struct dma_resv *resv) {
  if (resv->exclusive && drm_fence_is_signaled(resv->exclusive)) {
    drm_fence_put(resv->exclusive);
    resv->exclusive = NULL;
  }
  uint32_t out = 0;
  for (uint32_t i = 0; i < resv->shared_count; i++) {
    if (drm_fence_is_signaled(resv->shared[i]))
      drm_fence_put(resv->shared[i]);
    else
      resv->shared[out++] = resv->shared[i];
  }
  resv->shared_count = out;
}

int dma_resv_add_fence(struct dma_resv *resv, struct drm_fence *fence,
                       bool write) {
  if (!resv || !fence)
    return -EINVAL;
  uint64_t flags;
  spin_lock_irqsave(&resv->lock, &flags);
  dma_resv_prune_locked(resv);
  if (!write && resv->shared_count == DMA_RESV_MAX_SHARED) {
    spin_unlock_irqrestore(&resv->lock, flags);
    return -ENOSPC;
  }
  drm_fence_get(fence);
  if (write) {
    drm_fence_put(resv->exclusive);
    resv->exclusive = fence;
    for (uint32_t i = 0; i < resv->shared_count; i++)
      drm_fence_put(resv->shared[i]);
    resv->shared_count = 0;
  } else {
    resv->shared[resv->shared_count++] = fence;
  }
  resv->generation++;
  spin_unlock_irqrestore(&resv->lock, flags);
  return 0;
}

int dma_resv_wait(struct dma_resv *resv, bool write, uint64_t timeout_ns) {
  if (!resv)
    return -EINVAL;
  struct drm_fence *snapshot[DMA_RESV_MAX_SHARED + 1];
  uint32_t count = 0;
  uint64_t flags;
  spin_lock_irqsave(&resv->lock, &flags);
  dma_resv_prune_locked(resv);
  if (resv->exclusive) {
    snapshot[count++] = resv->exclusive;
    drm_fence_get(resv->exclusive);
  }
  if (write)
    for (uint32_t i = 0; i < resv->shared_count; i++) {
      snapshot[count++] = resv->shared[i];
      drm_fence_get(resv->shared[i]);
    }
  spin_unlock_irqrestore(&resv->lock, flags);

  int rc = 0;
  for (uint32_t i = 0; i < count; i++) {
    if (!rc)
      rc = drm_fence_wait(snapshot[i], timeout_ns);
    drm_fence_put(snapshot[i]);
  }
  return rc;
}

struct drm_fence *dma_resv_export_fence(struct dma_resv *resv, bool write) {
  if (!resv)
    return NULL;
  uint64_t flags;
  spin_lock_irqsave(&resv->lock, &flags);
  dma_resv_prune_locked(resv);
  struct drm_fence *fence = resv->exclusive;
  if (write && !fence && resv->shared_count)
    fence = resv->shared[0];
  if (fence)
    drm_fence_get(fence);
  spin_unlock_irqrestore(&resv->lock, flags);
  return fence ? fence : drm_fence_create(true);
}

bool dma_resv_ready(struct dma_resv *resv, bool write) {
  if (!resv)
    return false;
  uint64_t flags;
  spin_lock_irqsave(&resv->lock, &flags);
  dma_resv_prune_locked(resv);
  bool ready = !resv->exclusive && (!write || !resv->shared_count);
  spin_unlock_irqrestore(&resv->lock, flags);
  return ready;
}

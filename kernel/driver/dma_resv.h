/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef KERNEL_DRIVER_DMA_RESV_H
#define KERNEL_DRIVER_DMA_RESV_H

#include <stdbool.h>
#include <stdint.h>

#include "kernel/xcore/spinlock.h"

struct drm_fence;

#define DMA_RESV_MAX_SHARED 16

struct dma_resv {
  spinlock lock;
  struct drm_fence *exclusive;
  struct drm_fence *shared[DMA_RESV_MAX_SHARED];
  uint32_t shared_count;
  uint64_t generation;
};

void dma_resv_init(struct dma_resv *resv);
void dma_resv_fini(struct dma_resv *resv);
int dma_resv_wait(struct dma_resv *resv, bool write, uint64_t timeout_ns);
int dma_resv_add_fence(struct dma_resv *resv, struct drm_fence *fence,
                       bool write);
struct drm_fence *dma_resv_export_fence(struct dma_resv *resv, bool write);
bool dma_resv_ready(struct dma_resv *resv, bool write);

#endif

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_BSD_DEV_MMAP_H
#define KERNEL_BSD_DEV_MMAP_H

#include "kernel/xcore/mm_types.h"
#include <stdint.h>

#define DEV_MMAP_CACHE_UC 0x1u

struct dev_mmap_request {
  uint64_t addr;
  uint64_t length;
  uint64_t offset;
  uint32_t prot;
  uint32_t flags;
};

struct dev_mmap_backing {
  void *owner;
  const struct vma_owner_ops *owner_ops;
  struct page **pages;
  uint32_t page_count;
  uint32_t cache_flags;
};

// prepare owns one owner reference in backing. Commit consumes it on success;
// callers use abort after every failed prepare/commit path.
void dev_mmap_abort(struct dev_mmap_backing *backing);
int64_t dev_mmap_commit(mm *address_space, uint64_t *pml4,
                        const struct dev_mmap_request *request,
                        struct dev_mmap_backing *backing);

#endif

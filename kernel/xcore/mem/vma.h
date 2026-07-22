/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_XCORE_MEM_VMA_H
#define KERNEL_XCORE_MEM_VMA_H

// VMA (mmap_region) list primitives. S10 keeps the list sorted by vaddr
// (ascending) and provides interval find / gap find / split / merge helpers.
// Callers must hold mm->mmap_lock.

#include "kernel/xcore/mm_types.h"
#include <stdbool.h>
#include <stdint.h>

// Return the region containing addr ([vaddr, vaddr+size)), or NULL.
mmap_region *vma_find(mm *mm, uint64_t addr);

// Find a free interval of length >= len. Try [hint, hint+len) first; on
// conflict, bump-scan from max(hint, mmap_brk). Returns the start vaddr, or 0
// if none found (caller returns -ENOMEM). Pure query; does not touch mm.
uint64_t vma_find_gap(mm *mm, uint64_t len, uint64_t hint);

// Insert region sorted by vaddr. Returns 0, or -EEXIST on vaddr collision.
// Caller has allocated and fully initialized the region.
int vma_insert_sorted(mm *mm, mmap_region *region);

// Split [addr, addr+size) out of region. region may become up to three pieces
// (front / mid / tail). Returns the mid piece (for the caller to mutate), or
// NULL on bad args / OOM (region left intact). shm_obj refcount is NOT bumped:
// splitting one mapping into pieces does not change the mapping-instance count
// (munmap of a piece does the shm_put).
mmap_region *vma_split(mm *mm, mmap_region *region, uint64_t addr,
                       uint64_t size);

// Try to merge region with an adjacent anonymous-private neighbor (same
// prot+flags, contiguous vaddr). Returns the resulting region. Only merges
// anonymous private mappings (fd==-1 && shm_obj==NULL && phys==0).
mmap_region *vma_merge(mm *mm, mmap_region *region);

// --- S11: mmap addr hint / MAP_FIXED support ---

// True if [start, start+len) overlaps any existing region. Used by
// MAP_FIXED_NOREPLACE conflict detection.
bool vma_overlaps_any(mm *mm, uint64_t start, uint64_t len);

// Unmap every mapping overlapping [addr, addr+len): fully-contained regions
// are dropped, partially-overlapping ones are split first (front/tail residue
// preserved). Returns 0, or -ENOMEM if a vma_split OOMs (already-unmapped
// pieces are not rolled back). Caller holds mm->mmap_lock.
int vma_unmap_range(mm *mm, uint64_t *pml4, uint64_t addr, uint64_t len);

// Pick the placement vaddr per mmap addr-hint / MAP_FIXED / MAP_FIXED_NOREPLACE
// semantics (see S11). Returns the vaddr as a non-negative int64_t, or a
// negative -errno (-EEXIST / -ENOMEM). pml4 is the process page-table root
// (needed for the MAP_FIXED overlap-unmap). Caller holds mm->mmap_lock.
int64_t vma_pick_addr(mm *mm, uint64_t *pml4, uint64_t addr, uint64_t len,
                      uint32_t flags, uint64_t hint);

#endif // KERNEL_XCORE_MEM_VMA_H

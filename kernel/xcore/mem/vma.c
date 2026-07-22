/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// VMA (mmap_region) list primitives — sorted-by-vaddr single linked list with
// interval find / gap find / split / merge. S10 keeps all existing call paths
// behaviorally identical; these helpers replace the old head-insert + linear
// scans. All callers hold mm->mmap_lock.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kernel/xcore/kpi.h"
#include "kernel/xcore/mem/vma.h"
#include <xos/errno.h>

// Matches sys_mprotect's user-space upper bound (syscall.c). Beyond this the
// address space belongs to the kernel.
#define USER_VMA_UPPER_BOUND 0x800000000000ULL

mmap_region *vma_find(mm *mm, uint64_t addr) {
  for (mmap_region *mr = mm->mmap_regions; mr; mr = mr->next) {
    if (addr < mr->vaddr)
      return NULL; // sorted list: past addr without a hit
    if (addr < mr->vaddr + mr->size)
      return mr; // hit [vaddr, vaddr+size)
  }
  return NULL;
}

int vma_insert_sorted(mm *mm, mmap_region *region) {
  mmap_region **pp = &mm->mmap_regions;
  while (*pp && (*pp)->vaddr < region->vaddr)
    pp = &(*pp)->next;
  if (*pp && (*pp)->vaddr == region->vaddr)
    return -EEXIST;
  region->next = *pp;
  *pp = region;
  return 0;
}

// True if [start, start+len) overlaps any region. Sorted list lets us stop
// early once a region starts at or past the interval end.
static bool vma_overlaps(mm *mm, uint64_t start, uint64_t len) {
  uint64_t end = start + len;
  for (mmap_region *mr = mm->mmap_regions; mr; mr = mr->next) {
    if (mr->vaddr >= end)
      return false;
    if (mr->vaddr + mr->size > start)
      return true;
  }
  return false;
}

uint64_t vma_find_gap(mm *mm, uint64_t len, uint64_t hint) {
  if (hint && !vma_overlaps(mm, hint, len))
    return hint;
  uint64_t cur = (hint < mm->mmap_brk) ? mm->mmap_brk : hint;
  for (mmap_region *mr = mm->mmap_regions; mr; mr = mr->next) {
    if (cur + len <= mr->vaddr)
      return cur;
    if (mr->vaddr + mr->size > cur)
      cur = mr->vaddr + mr->size;
  }
  if (cur + len <= USER_VMA_UPPER_BOUND)
    return cur;
  return 0;
}

mmap_region *vma_split(mm *mm, mmap_region *r, uint64_t addr, uint64_t size) {
  if (!r || size == 0 || addr < r->vaddr || addr + size > r->vaddr + r->size)
    return NULL;

  uint64_t r_end = r->vaddr + r->size;
  uint64_t delta = addr - r->vaddr;
  uint64_t mid_off = r->offset + delta;

  // Middle piece.
  mmap_region *mid = (mmap_region *)kmalloc(sizeof(mmap_region));
  if (!mid)
    return NULL;
  *mid = *r;
  mid->vaddr = addr;
  mid->size = size;
  mid->offset = mid_off;
  mid->phys = r->phys ? (r->phys + delta) : 0;
  mid->next = NULL;

  // Tail piece (if any).
  mmap_region *tail = NULL;
  if (addr + size < r_end) {
    tail = (mmap_region *)kmalloc(sizeof(mmap_region));
    if (!tail) {
      kfree(mid);
      return NULL;
    }
    *tail = *r;
    uint64_t t_delta = (addr + size) - r->vaddr;
    tail->vaddr = addr + size;
    tail->size = r_end - (addr + size);
    tail->offset = r->offset + t_delta;
    tail->phys = r->phys ? (r->phys + t_delta) : 0;
    tail->next = NULL;
  }

  if (addr == r->vaddr) {
    // No front piece: replace r in the list with mid (-> tail).
    mmap_region **pp = &mm->mmap_regions;
    while (*pp != r)
      pp = &(*pp)->next;
    mid->next = tail;
    *pp = mid;
    kfree(r);
  } else {
    // Shrink r to the front piece.
    r->size = delta;
    mmap_region *after = r->next;
    r->next = mid;
    mid->next = tail ? tail : after;
    if (tail)
      tail->next = after;
  }
  return mid;
}

mmap_region *vma_merge(mm *mm, mmap_region *r) {
  if (!r || r->fd != -1 || r->shm_obj || r->phys)
    return r;

  // Merge with the next region if anonymous-private, same prot/flags, adjacent.
  mmap_region *n = r->next;
  if (n && n->fd == -1 && !n->shm_obj && !n->phys && n->prot == r->prot &&
      n->flags == r->flags && r->vaddr + r->size == n->vaddr) {
    r->size += n->size;
    r->next = n->next;
    kfree(n);
  }

  // Merge with the previous region (find it; sorted list, O(n)).
  mmap_region **pp = &mm->mmap_regions;
  while (*pp && (*pp)->next != r)
    pp = &(*pp)->next;
  mmap_region *p = *pp;
  if (p && p->fd == -1 && !p->shm_obj && !p->phys && p->prot == r->prot &&
      p->flags == r->flags && p->vaddr + p->size == r->vaddr) {
    p->size += r->size;
    p->next = r->next;
    kfree(r);
    return p;
  }
  return r;
}

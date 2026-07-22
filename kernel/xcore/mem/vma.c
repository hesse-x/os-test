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

#include "arch/x64/utils.h"
#include "kernel/xcore/kpi.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/mem/vma.h"

#include <xos/errno.h>
#include <xos/mman.h>
#include <xos/page.h>

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

// ===================== S11: mmap addr hint / MAP_FIXED support
// =====================

// Does [start, start+len) overlap any region? Sorted list lets us stop early.
// Public for MAP_FIXED_NOREPLACE conflict detection.
bool vma_overlaps_any(mm *mm, uint64_t start, uint64_t len) {
  return vma_overlaps(mm, start, len);
}

// Release one region's pages + PTEs, unlink it from the sorted list, and free
// the descriptor. Mirrors sys_munmap's two-branch release:
//  - anonymous (shm_obj==NULL && phys==0): unmap_user_pages refcount-decs and
//    frees the backing pages.
//  - SHM (shm_obj != NULL): clear PTEs only (pages are SHM-owned), then
//    shm_put the mapping-instance reference.
//  - MAP_PHYSICAL (phys != 0): clear PTEs only (phys is external MMIO).
// Caller holds mm->mmap_lock.
static void free_one_region(mm *mm, uint64_t *pml4, mmap_region *r) {
  size_t npages = r->size / PAGE_SIZE;

  if (r->shm_obj || r->phys) {
    // SHM / MAP_PHYSICAL: clear leaf PTEs without freeing the backing pages.
    for (size_t i = 0; i < npages; i++) {
      uint64_t va = r->vaddr + i * PAGE_SIZE;
      uint64_t *pdpt = ensure_pd(pml4, va);
      if (!pdpt)
        continue;
      uint64_t *pd = ensure_pt_in_pd(pdpt, va, 2);
      if (!pd)
        continue;
      uint64_t *pt = ensure_pt_in_pd(pd, va, 1);
      if (!pt)
        continue;
      uint64_t pt_idx = (va >> 12) & 0x1FF;
      pt[pt_idx] = 0;
      // PTE is now 0, but a stale TLB entry may still hold the old translation
      // (vma_unmap_range is the MAP_FIXED overlap-unmap; the caller — sys_mmap
      // SHM/anon — immediately writes a fresh PTE to this VA via
      // map_user_page_direct, which refuses to overwrite a present PTE but
      // cannot evict a cached one). Flush so the new mapping takes effect.
      invlpg(va);
    }
    if (r->shm_obj)
      shm_put(r->shm_obj);
  } else {
    // Anonymous: unmap_user_pages refcount-decs and frees pages + clears PTEs.
    for (size_t i = 0; i < npages; i++) {
      uint64_t va = r->vaddr + i * PAGE_SIZE;
      unmap_user_pages(pml4, va, va + PAGE_SIZE, 1);
      invlpg(
          va); // see the SHM/phys branch: stale TLB would shadow the new PTE.
    }
  }

  // Unlink from the sorted list.
  mmap_region **pp = &mm->mmap_regions;
  while (*pp != r)
    pp = &(*pp)->next;
  *pp = r->next;
  kfree(r);
}

// Unmap every existing mapping overlapping [addr, addr+len). Fully-contained
// regions are dropped; partially-overlapping ones are split first (front/tail
// residue preserved via vma_split), then the overlapping piece is freed. This
// is the MAP_FIXED overlap-unmap and the basis for S13's partial munmap.
// Returns 0, or -ENOMEM if a vma_split OOMs (already-unmapped pieces are not
// rolled back — matches Linux do_munmap). Caller holds mm->mmap_lock.
int vma_unmap_range(mm *mm, uint64_t *pml4, uint64_t addr, uint64_t len) {
  uint64_t end = addr + len;
  mmap_region *cur = mm->mmap_regions;
  while (cur) {
    mmap_region *next = cur->next;
    if (cur->vaddr >= end)
      break; // sorted list: past the interval
    if (cur->vaddr + cur->size <= addr) {
      cur = next;
      continue; // no overlap
    }

    if (cur->vaddr < addr) {
      // Front residue: split [addr, min(cur->end, end)) out as the mid piece.
      uint64_t split_len = (cur->vaddr + cur->size < end)
                               ? (cur->vaddr + cur->size - addr)
                               : (end - addr);
      mmap_region *mid = vma_split(mm, cur, addr, split_len);
      if (!mid)
        return -ENOMEM;
      free_one_region(mm, pml4, mid); // front residue (cur) stays
      cur = next;
      continue;
    }

    if (cur->vaddr + cur->size > end) {
      // No front residue, but a tail residue: split [cur->vaddr, end) out.
      mmap_region *mid = vma_split(mm, cur, cur->vaddr, end - cur->vaddr);
      if (!mid)
        return -ENOMEM;
      free_one_region(mm, pml4, mid); // tail residue (cur's remainder) stays
      cur = next;
      continue;
    }

    // Fully contained in [addr, end).
    free_one_region(mm, pml4, cur);
    cur = next;
  }
  return 0;
}

// Pick the placement vaddr per the mmap addr-hint / MAP_FIXED semantics:
//  - MAP_FIXED:           vma_unmap_range(addr,len) then return addr.
//  - MAP_FIXED_NOREPLACE: -EEXIST on any overlap, else addr (no unmapping).
//  - neither:             vma_find_gap(len, hint); 0 → -ENOMEM.
// Returns the vaddr as a non-negative int64_t, or a negative -errno. The
// caller advances mmap_brk only when the returned vaddr equals mmap_brk.
// Caller holds mm->mmap_lock.
int64_t vma_pick_addr(mm *mm, uint64_t *pml4, uint64_t addr, uint64_t len,
                      uint32_t flags, uint64_t hint) {
  if (flags & MAP_FIXED) {
    int r = vma_unmap_range(mm, pml4, addr, len);
    if (r < 0)
      return r;
    return (int64_t)addr;
  }
  if (flags & MAP_FIXED_NOREPLACE) {
    if (vma_overlaps_any(mm, addr, len))
      return -EEXIST;
    return (int64_t)addr;
  }
  uint64_t v = vma_find_gap(mm, len, hint);
  return v ? (int64_t)v : (int64_t)-ENOMEM;
}

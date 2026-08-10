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
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/kpi.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/mem/vma.h"

#include <xos/errno.h>
#include <xos/mman.h>
#include <xos/page.h>
#include <xos/signal.h>

vma_writeback_fn vma_writeback_hook = NULL;

static bool overlaps_signal_trampoline(uint64_t start, uint64_t len) {
  if (len == 0 || start > UINT64_MAX - len)
    return false;
  uint64_t end = start + len;
  return start < SIG_TRAMPOLINE_ADDR + PAGE_SIZE && end > SIG_TRAMPOLINE_ADDR;
}

void vma_reset_readahead(mmap_region *region) {
  if (!region)
    return;
  region->ra_last_page = UINT64_MAX;
  region->ra_next_page = 0;
  region->ra_window_pages = 0;
  region->ra_sequential_faults = 0;
}

// USER_VMA_UPPER_BOUND now lives in vma.h (shared with sys_mremap).

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

int vma_attach_owner(mmap_region *region, void *owner,
                     const struct vma_owner_ops *ops) {
  int rc = vma_adopt_owner(region, owner, ops);
  if (rc)
    return rc;
  ops->get(owner);
  return 0;
}

int vma_adopt_owner(mmap_region *region, void *owner,
                    const struct vma_owner_ops *ops) {
  if (!region || !owner || !ops || !ops->get || !ops->put ||
      (region->flags & KMAP_VMA_OWNER))
    return -EINVAL;
  region->owner = owner;
  region->owner_ops = ops;
  region->flags |= KMAP_VMA_OWNER;
  return 0;
}

void vma_owner_get(mmap_region *region) {
  if (region && (region->flags & KMAP_VMA_OWNER))
    region->owner_ops->get(region->owner);
}

void vma_owner_put(mmap_region *region) {
  if (region && (region->flags & KMAP_VMA_OWNER)) {
    region->flags &= ~KMAP_VMA_OWNER;
    region->owner_ops->put(region->owner);
    region->owner = NULL;
    region->owner_ops = NULL;
  }
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

static uint64_t first_mapped_page(mm *mm, uint64_t start, uint64_t len) {
  // The trampoline is a kernel-owned reservation, not a normal mmap_region.
  // Treat it as occupied even if its PTE is temporarily absent so a failed
  // exec/map cannot make later allocations straddle the fixed signal ABI page.
  if (overlaps_signal_trampoline(start, len))
    return SIG_TRAMPOLINE_ADDR;
  uint64_t end = start + len;
  for (uint64_t va = start; va < end; va += PAGE_SIZE) {
    if (lookup_pte(mm->cr3, va))
      return va;
  }
  return 0;
}

uint64_t vma_find_gap(mm *mm, uint64_t len, uint64_t hint) {
  if (hint && len <= USER_VMA_UPPER_BOUND - hint &&
      !vma_overlaps(mm, hint, len) && !first_mapped_page(mm, hint, len))
    return hint;

  uint64_t cur = (hint < mm->mmap_brk) ? mm->mmap_brk : hint;
  for (;;) {
    if (cur >= USER_VMA_UPPER_BOUND || len > USER_VMA_UPPER_BOUND - cur)
      return 0;

    bool moved = false;
    for (mmap_region *mr = mm->mmap_regions; mr; mr = mr->next) {
      uint64_t mr_end = mr->vaddr + mr->size;
      if (mr_end <= cur)
        continue;
      if (mr->vaddr >= cur + len)
        break;
      cur = mr_end;
      moved = true;
      break;
    }
    if (moved)
      continue;

    uint64_t mapped = first_mapped_page(mm, cur, len);
    if (!mapped)
      return cur;
    cur = mapped + PAGE_SIZE;
  }
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
  vma_reset_readahead(mid);
  mid->vaddr = addr;
  mid->size = size;
  mid->offset = mid_off;
  mid->phys = r->phys ? (r->phys + delta) : 0;
  mid->next = NULL;
  // mid is a new owner of r's backing references: every region struct owns
  // exactly one ref (see free_one_region / copy_mmap_regions). r keeps its own
  // refs until it is freed or shrunk below.
  if (mid->shm_obj)
    shm_get(mid->shm_obj);
  if (mid->flags & KMAP_SHM_MAYWRITE)
    atomic_inc(&mid->shm_obj->writable_shared_mappings);
  if (mid->inode)
    inode_get(mid->inode);
  if (mid->shm_private_src)
    shm_get(mid->shm_private_src);
  vma_owner_get(mid);

  // Tail piece (if any).
  mmap_region *tail = NULL;
  if (addr + size < r_end) {
    tail = (mmap_region *)kmalloc(sizeof(mmap_region));
    if (!tail) {
      if (mid->flags & KMAP_SHM_MAYWRITE)
        atomic_dec(&mid->shm_obj->writable_shared_mappings);
      if (mid->shm_obj)
        shm_put(mid->shm_obj);
      if (mid->inode)
        inode_put(mid->inode);
      if (mid->shm_private_src)
        shm_put(mid->shm_private_src);
      vma_owner_put(mid);
      kfree(mid);
      return NULL;
    }
    *tail = *r;
    vma_reset_readahead(tail);
    uint64_t t_delta = (addr + size) - r->vaddr;
    tail->vaddr = addr + size;
    tail->size = r_end - (addr + size);
    tail->offset = r->offset + t_delta;
    tail->phys = r->phys ? (r->phys + t_delta) : 0;
    tail->next = NULL;
    if (tail->shm_obj)
      shm_get(tail->shm_obj);
    if (tail->flags & KMAP_SHM_MAYWRITE)
      atomic_inc(&tail->shm_obj->writable_shared_mappings);
    if (tail->inode)
      inode_get(tail->inode);
    if (tail->shm_private_src)
      shm_get(tail->shm_private_src);
    vma_owner_get(tail);
  }

  if (addr == r->vaddr) {
    // No front piece: replace r in the list with mid (-> tail). r is destroyed,
    // so release its backing refs (mid/tail each took their own).
    mmap_region **pp = &mm->mmap_regions;
    while (*pp != r)
      pp = &(*pp)->next;
    mmap_region *after = r->next;
    mid->next = tail ? tail : after;
    if (tail)
      tail->next = after;
    *pp = mid;
    if (r->flags & KMAP_SHM_MAYWRITE)
      atomic_dec(&r->shm_obj->writable_shared_mappings);
    if (r->shm_obj)
      shm_put(r->shm_obj);
    if (r->inode)
      inode_put(r->inode);
    if (r->shm_private_src)
      shm_put(r->shm_private_src);
    vma_owner_put(r);
    kfree(r);
  } else {
    // Shrink r to the front piece.
    r->size = delta;
    vma_reset_readahead(r);
    mmap_region *after = r->next;
    r->next = mid;
    mid->next = tail ? tail : after;
    if (tail)
      tail->next = after;
  }
  return mid;
}

mmap_region *vma_merge(mm *mm, mmap_region *r) {
  // Only purely-anonymous regions (no fd, no SHM, no PHYSICAL, no file-backed
  // inode/shm_private_src ref) are eligible — merged regions are kfree'd
  // without per-struct ref drop, so anything holding an inode/shm ref must be
  // excluded to avoid leaks.
  if (!r || r->fd != -1 || r->shm_obj || r->phys || r->inode ||
      (r->flags & KMAP_VMA_OWNER) || r->shm_private_src)
    return r;

  // Merge with the next region if anonymous-private, same prot/flags, adjacent.
  mmap_region *n = r->next;
  if (n && n->fd == -1 && !n->shm_obj && !n->phys && !n->inode &&
      !(n->flags & KMAP_VMA_OWNER) && !n->shm_private_src &&
      n->prot == r->prot && n->flags == r->flags &&
      r->vaddr + r->size == n->vaddr) {
    r->size += n->size;
    r->next = n->next;
    kfree(n);
  }

  // Merge with the previous region (find it; sorted list, O(n)).
  mmap_region **pp = &mm->mmap_regions;
  while (*pp && (*pp)->next != r)
    pp = &(*pp)->next;
  mmap_region *p = *pp;
  if (p && p->fd == -1 && !p->shm_obj && !p->phys && !p->inode &&
      !(p->flags & KMAP_VMA_OWNER) && !p->shm_private_src &&
      p->prot == r->prot && p->flags == r->flags &&
      p->vaddr + p->size == r->vaddr) {
    p->size += r->size;
    p->next = r->next;
    kfree(r);
    return p;
  }
  return r;
}

// ===================== S13: mprotect interval splitting =====================

// Set prot on every mapping overlapping [addr, addr+len): split out the
// overlapping piece from partially-overlapping regions (front/tail residue
// kept) and update the middle piece's prot. Fully-contained regions get their
// prot changed in place. This only touches region metadata — the caller has
// already (or will) rewrite the leaf PTEs page-by-page. Returns 0, or -ENOMEM
// if a vma_split OOMs (already-changed regions are not rolled back, matching
// Linux's partial-failure mprotect). Caller holds mm->mmap_lock.
int vma_protect_range(mm *mm, uint64_t addr, uint64_t len, uint32_t prot) {
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
      mid->prot = prot;
      cur = next; // front residue (cur) stays with its old prot
      continue;
    }

    if (cur->vaddr + cur->size > end) {
      // No front residue, but a tail residue: split [cur->vaddr, end) out.
      mmap_region *mid = vma_split(mm, cur, cur->vaddr, end - cur->vaddr);
      if (!mid)
        return -ENOMEM;
      mid->prot = prot;
      cur = next;
      continue;
    }

    // Fully contained in [addr, end): change prot in place, no split needed.
    cur->prot = prot;
    cur = next;
  }

  // No cross-region merge: a middle piece whose prot differs from its
  // residues cannot merge with them, and conservatively merging two regions
  // that happen to now share prot risks vma_merge's anonymous-private-only
  // rule misfiring on adjacent file-backed holes. The fragmentation cost is
  // negligible; revisit if mprotect churn becomes heavy.
  return 0;
}

// ===================== S11: mmap addr hint / MAP_FIXED support
// =====================

// Does [start, start+len) overlap any region? Sorted list lets us stop early.
// Public for MAP_FIXED_NOREPLACE conflict detection.
bool vma_overlaps_any(mm *mm, uint64_t start, uint64_t len) {
  if (overlaps_signal_trampoline(start, len))
    return true;
  return vma_overlaps(mm, start, len);
}

// Release one region's pages + PTEs, unlink it from the sorted list, and free
// the descriptor. Mirrors sys_munmap's two-branch release:
//  - anonymous (shm_obj==NULL && phys==0): unmap_user_pages refcount-decs and
//    frees the backing pages.
//  - SHM, MAP_PHYSICAL, and owner-backed device VMAs: clear PTEs only because
//    their pages are released by the backing owner, not by the address space.
// Caller holds mm->mmap_lock.
static void free_one_region(mm *mm, uint64_t *pml4, mmap_region *r) {
  size_t npages = r->size / PAGE_SIZE;

  // Regular-file MAP_SHARED faults currently use private user pages. Copy
  // dirty pages back into the page cache before unmapping those pages.
  if (vma_writeback_hook)
    (void)vma_writeback_hook(mm->cr3, r);

  if (r->shm_obj || r->phys || (r->flags & KMAP_VMA_OWNER)) {
    // Externally owned mappings only drop this address space's PTE view.
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
    if (r->flags & KMAP_SHM_MAYWRITE)
      atomic_dec(&r->shm_obj->writable_shared_mappings);
    if (r->shm_obj)
      shm_put(r->shm_obj);
    if (r->flags & KMAP_DMA_OWNED) {
      struct page *page = &bfc_frames[r->phys / PAGE_SIZE];
      bfc_free_page(page, npages);
    }
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
  // Drop file-backed mmap refs owned by this region struct (S12). Every region
  // with a non-NULL inode/shm_private_src owns exactly one reference (taken in
  // sys_mmap_file_backed / vma_split / copy_mmap_regions), released here.
  if (r->inode)
    inode_put(r->inode);
  if (r->shm_private_src)
    shm_put(r->shm_private_src);
  vma_owner_put(r);
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
// placement helper does not mutate mmap_brk; callers apply the policy for
// default allocations versus explicit hints after the mapping succeeds.
// Caller holds mm->mmap_lock.
int64_t vma_pick_addr(mm *mm, uint64_t *pml4, uint64_t addr, uint64_t len,
                      uint32_t flags, uint64_t hint) {
  if (flags & MAP_FIXED) {
    if (overlaps_signal_trampoline(addr, len))
      return -EINVAL;
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

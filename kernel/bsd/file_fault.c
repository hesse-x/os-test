/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// S12: file-backed mmap page-in (fault-on-demand).
//
// Registered as the Xcore fault_handler hook (kernel/xcore/trap.c). On a #PF
// for a vaddr that belongs to a file-backed (or memfd-MAP_PRIVATE) mmap_region,
// read the backing page into a fresh private user page and map it per the
// region's prot, then return 1 so the faulting instruction retries.
//
// Design notes (see refact_syscall/S12_mmap_private_fd_filemap.md):
//  - Fault-on-demand: sys_mmap_file_backed records only region metadata
//    (vaddr/size/offset/prot/inode); no pages are allocated up front.
//  - The region holds an inode_get reference, so the mapping survives
//    close(fd) — the fault handler reads mr->inode directly, never fd_lookup.
//  - Private pages are mapped RW (when PROT_WRITE) directly, with NO PTE_COW.
//    A file-backed MAP_PRIVATE page is already an exclusive private copy
//    (freshly bfc_alloc_page'd, refcount==1, memcpy'd from the page cache), so
//    there is nothing shared to write-protect. On fork, copy_page_table's
//    normal RW-page COW path (refcount_inc + rewrite both sides RO|COW) takes
//    over and gives correct parent/child isolation — identical to anonymous
//    pages, with no copy_page_table changes needed.
//  - MAP_SHARED+fd is a simplified read path: each fault still copies the page
//    into a private page (no shared page-cache mapping / no write-back yet).
//    Read correctness (config/font/ELF .text) is the S12 acceptance bar; shared
//    dirty write-back is deferred to todo.

#include "kernel/bsd/file_fault.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arch/x64/paging.h"
#include "arch/x64/utils.h"
#include "kernel/bsd/inode.h"
#include "kernel/bsd/page_cache.h"
#include "kernel/xcore/kpi.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/mem/vma.h"
#include "kernel/xcore/mm_types.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/trap.h"
#include "kernel/xcore/xtask.h"
#include "utils/macro.h"

#include <xos/errno.h>
#include <xos/mman.h>
#include <xos/page.h>

// Compute the PTE flags for a freshly-faulted private page from the region's
// prot. File-backed private pages are exclusive copies, so a writable mapping
// maps RW directly (no PTE_COW — see file header). NX unless PROT_EXEC.
static uint64_t file_fault_pte_flags(uint32_t prot) {
  uint64_t flags = PTE_PRESENT | PTE_USER;
  if (prot & PROT_WRITE)
    flags |= PTE_RW;
  if (!(prot & PROT_EXEC))
    flags |= PTE_NX;
  return flags;
}

// Map one private user page at page_addr -> user_phys with pte_flags. Returns
// true on success. On failure the caller owns user_phys (must free it).
static bool file_fault_map(uint64_t cr3, uint64_t page_addr, uint64_t user_phys,
                           uint64_t pte_flags) {
  uint64_t *pml4 = (__force uint64_t *)phys_to_virt((__force phys_addr_t)cr3);
  if (!map_user_page_direct(pml4, page_addr, user_phys, pte_flags)) {
    printk(LOG_WARN, "file_fault: map failed addr=0x%lx\n",
           (unsigned long)page_addr);
    return false;
  }
  return true;
}

int file_fault_handler(uint64_t fault_addr, xtask *t) {
  uint64_t page_addr = ALIGN_DOWN(fault_addr, PAGE_SIZE);
  mmap_region *mr = vma_find(t->mm, page_addr);
  if (!mr)
    return 0; // not a known mapping — let #PF deliver SIGSEGV

  uint64_t fault_off = mr->offset + (page_addr - mr->vaddr);

  // Already mapped (present)? Then this is not a file-backed demand-fault
  // (e.g. a PROT_NONE guard, or a genuine permission violation). Hand it back.
  uint64_t *pte = lookup_pte(t->mm->cr3, page_addr);
  if (pte && pte_present(*pte))
    return 0;

  // ---- FD_REGULAR file-backed mapping: page-in via the page cache ----
  if (mr->inode) {
    uint64_t mmap_flags;
    spin_lock_irqsave(&t->mm->mmap_lock, &mmap_flags);
    mr = vma_find(t->mm, page_addr);
    if (!mr || !mr->inode) {
      spin_unlock_irqrestore(&t->mm->mmap_lock, mmap_flags);
      return FAULT_NOT_HANDLED;
    }
    fault_off = mr->offset + (page_addr - mr->vaddr);
    uint64_t page_idx = fault_off / PAGE_SIZE;
    uint32_t window = 1;
    if (mr->ra_sequential_faults == 0) {
      mr->ra_sequential_faults = 1;
    } else if (mr->ra_last_page != UINT64_MAX &&
               page_idx == mr->ra_last_page + 1) {
      if (mr->ra_window_pages == 0) {
        mr->ra_window_pages = 4;
        mr->ra_next_page = page_idx + 4;
      } else if (page_idx == mr->ra_next_page) {
        mr->ra_window_pages = mr->ra_window_pages < PAGE_CACHE_RA_MAX_PAGES
                                  ? mr->ra_window_pages * 2
                                  : PAGE_CACHE_RA_MAX_PAGES;
        mr->ra_next_page = page_idx + mr->ra_window_pages;
      }
      mr->ra_sequential_faults++;
      window = mr->ra_window_pages ? mr->ra_window_pages : 1;
    } else {
      mr->ra_window_pages = 0;
      mr->ra_next_page = 0;
      mr->ra_sequential_faults = 1;
    }
    mr->ra_last_page = page_idx;

    uint64_t vma_end = mr->vaddr + mr->size;
    uint64_t vma_pages = (vma_end - page_addr) / PAGE_SIZE;
    if (vma_pages == 0)
      vma_pages = 1;
    if (window > vma_pages)
      window = (uint32_t)vma_pages;

    uint64_t snap_vaddr = mr->vaddr;
    uint64_t snap_size = mr->size;
    uint64_t snap_offset = mr->offset;
    uint32_t snap_prot = mr->prot;
    struct inode *ip = inode_get(mr->inode);
    spin_unlock_irqrestore(&t->mm->mmap_lock, mmap_flags);

    struct cache_page *cp;
    int fill_rc =
        page_cache_get_ra(ip, page_idx, window, PAGE_CACHE_RA_MMAP, &cp);
    if (fill_rc) {
      printk(LOG_WARN, "file_fault: page_cache_fill failed inode=%p idx=%lu\n",
             (void *)ip, (unsigned long)page_idx);
      inode_put(ip);
      return fill_rc == -EIO ? FAULT_IO_ERROR : FAULT_NOT_HANDLED;
    }

    struct page *user_page = bfc_alloc_page(1);
    if (!user_page) {
      page_cache_release(cp);
      inode_put(ip);
      printk(LOG_WARN, "file_fault: OOM user page addr=0x%lx\n",
             (unsigned long)page_addr);
      return 0;
    }
    uint64_t user_phys = (__force uint64_t)page_to_phys(user_page);
    void *user_va =
        (__force void *)phys_to_virt((__force phys_addr_t)user_phys);

    __memcpy(user_va, cp->data, PAGE_SIZE);
    page_cache_release(cp);

    // Zero the sub-page tail beyond EOF on the last page so a short file
    // mmap'd into a full page reads zero past its end (Linux semantics within
    // the mapped page; mappings extending past EOF are rejected at mmap time).
    uint64_t file_size = ip->size;
    if (fault_off < file_size) {
      uint64_t page_end = fault_off + PAGE_SIZE;
      if (page_end > file_size) {
        size_t off_in_page = (size_t)(file_size - fault_off);
        __memset((uint8_t *)user_va + off_in_page, 0, PAGE_SIZE - off_in_page);
      }
    } else {
      // fault_off >= file_size only happens for a page fully beyond EOF; mmap
      // rejects offset+size > size, so this is defensive — zero the page.
      __memset(user_va, 0, PAGE_SIZE);
    }

    uint64_t pte_flags = file_fault_pte_flags(snap_prot);
    spin_lock_irqsave(&t->mm->mmap_lock, &mmap_flags);
    mmap_region *current = vma_find(t->mm, page_addr);
    bool same_mapping =
        current && current->inode == ip && current->vaddr == snap_vaddr &&
        current->size == snap_size && current->offset == snap_offset &&
        current->prot == snap_prot;
    pte = lookup_pte(t->mm->cr3, page_addr);
    if (same_mapping && pte && pte_present(*pte)) {
      spin_unlock_irqrestore(&t->mm->mmap_lock, mmap_flags);
      bfc_free_page(user_page, 1);
      inode_put(ip);
      return FAULT_HANDLED;
    }
    bool mapped = same_mapping &&
                  file_fault_map(t->mm->cr3, page_addr, user_phys, pte_flags);
    spin_unlock_irqrestore(&t->mm->mmap_lock, mmap_flags);
    if (!mapped)
      bfc_free_page(user_page, 1);
    inode_put(ip);
    return mapped ? FAULT_HANDLED : FAULT_NOT_HANDLED;
  }

  // ---- memfd MAP_PRIVATE mapping: COW-copy from the shm page list ----
  if (mr->shm_private_src) {
    struct shm *shm = mr->shm_private_src;
    size_t idx = (size_t)(fault_off / PAGE_SIZE);
    uint64_t src_phys;
    size_t npages = shm->npages;
    size_t list_pages = shm->page_list ? (size_t)shm->num_pages : 0;
    if (idx < npages) {
      src_phys = shm->phys + idx * PAGE_SIZE;
    } else if (idx - npages < list_pages) {
      src_phys = shm->page_list[idx - npages];
    } else {
      return 0; // beyond the memfd → SIGSEGV
    }

    struct page *user_page = bfc_alloc_page(1);
    if (!user_page) {
      printk(LOG_WARN, "file_fault: OOM user page (memfd) addr=0x%lx\n",
             (unsigned long)page_addr);
      return 0;
    }
    uint64_t user_phys = (__force uint64_t)page_to_phys(user_page);
    void *user_va =
        (__force void *)phys_to_virt((__force phys_addr_t)user_phys);
    void *src_va = (__force void *)phys_to_virt((__force phys_addr_t)src_phys);
    __memcpy(user_va, src_va, PAGE_SIZE);

    uint64_t pte_flags = file_fault_pte_flags(mr->prot);
    if (!file_fault_map(t->mm->cr3, page_addr, user_phys, pte_flags)) {
      bfc_free_page(user_page, 1);
      return 0;
    }
    return 1;
  }

  return 0; // anonymous / SHM / PHYSICAL — not a file-backed demand-fault
}

int file_shared_mmap_writeback(uint64_t cr3, mmap_region *region) {
  if (!region || !region->inode || !(region->flags & MAP_SHARED))
    return 0;

  int first_error = 0;
  uint64_t file_offset = region->offset;
  uint64_t file_size = region->inode->size;
  for (uint64_t delta = 0; delta < region->size; delta += PAGE_SIZE) {
    if (file_offset + delta >= file_size)
      break;

    uint64_t *pte = lookup_pte(cr3, region->vaddr + delta);
    if (!pte || !(*pte & PTE_DIRTY))
      continue;

    struct cache_page *cp =
        page_cache_fill(region->inode, (file_offset + delta) / PAGE_SIZE);
    if (!cp) {
      if (!first_error)
        first_error = -EIO;
      continue;
    }

    uint64_t remaining = file_size - (file_offset + delta);
    size_t bytes = remaining < PAGE_SIZE ? (size_t)remaining : PAGE_SIZE;
    uint64_t phys = *pte & PTE_PHYS_MASK;
    void *source = (__force void *)phys_to_virt((__force phys_addr_t)phys);
    __memcpy(cp->data, source, bytes);
    page_cache_mark_dirty(cp);
    int rc = page_cache_writeback(cp);
    page_cache_release(cp);
    if (rc) {
      if (!first_error)
        first_error = rc;
      continue;
    }

    *pte &= ~PTE_DIRTY;
  }
  return first_error;
}

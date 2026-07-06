/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// User-space page mapping functions
// Used by proc.c (process creation) and trap.c (sys_mmap/sys_munmap/sys_spawn)

#include "arch/x64/memlayout.h"
#include "arch/x64/paging.h"
#include "utils/macro.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/kpi.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/mm_types.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/trap.h"
#include "kernel/xcore/xtask.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <xos/errno.h>
#include <xos/shm.h>

// Lookup the leaf PTE for a given virtual address in a page table hierarchy.
// cr3_phys is the physical address of the PML4 (as stored in mm->cr3 or CR3).
// Returns pointer to the leaf PTE if all intermediate levels exist, NULL
// otherwise.
__attribute__((no_sanitize("kernel-address"))) uint64_t *
lookup_pte(uint64_t cr3_phys, uint64_t vaddr) {
  uint64_t *pml4 = (uint64_t *)phys_to_virt((__force phys_addr_t)cr3_phys);
  uint64_t pml4_idx = (vaddr >> 39) & 0x1FF;
  if (!(pml4[pml4_idx] & PTE_PRESENT))
    return NULL;

  uint64_t *pdpt = (uint64_t *)phys_to_virt(
      (__force phys_addr_t)(pml4[pml4_idx] & PTE_PHYS_MASK));
  uint64_t pdpt_idx = (vaddr >> 30) & 0x1FF;
  if (!(pdpt[pdpt_idx] & PTE_PRESENT))
    return NULL;

  uint64_t *pd = (uint64_t *)phys_to_virt(
      (__force phys_addr_t)(pdpt[pdpt_idx] & PTE_PHYS_MASK));
  uint64_t pd_idx = (vaddr >> 21) & 0x1FF;
  if (!(pd[pd_idx] & PTE_PRESENT))
    return NULL;

  uint64_t *pt = (uint64_t *)phys_to_virt(
      (__force phys_addr_t)(pd[pd_idx] & PTE_PHYS_MASK));
  uint64_t pt_idx = (vaddr >> 12) & 0x1FF;
  if (!(pt[pt_idx] & PTE_PRESENT))
    return NULL;

  return &pt[pt_idx];
}

// Ensure a PDPT entry exists for the given virtual address in user PML4.
// Returns the virtual address of the PD, or allocates a new one.
__attribute__((no_sanitize("kernel-address"))) uint64_t *
ensure_pd(uint64_t *new_pml4, uint64_t vaddr) {
  uint64_t pml4_idx = (vaddr >> 39) & 0x1FF;
  if (new_pml4[pml4_idx] & 0x01) {
    return (__force uint64_t *)phys_to_virt(
        (__force phys_addr_t)(new_pml4[pml4_idx] & 0x000FFFFFFFFFF000ULL));
  }
  // Allocate new PDPT
  struct page *pdpt_page = bfc_alloc_page(1);
  if (!pdpt_page)
    return NULL;
  uint64_t pdpt_phys = (__force uint64_t)page_to_phys(pdpt_page);
  uint64_t pdpt_virt =
      (__force uint64_t)phys_to_virt((__force phys_addr_t)pdpt_phys);
  uint64_t *pdpt = (uint64_t *)pdpt_virt;
  for (int i = 0; i < 512; i++) {
    pdpt[i] = 0;
  }
  new_pml4[pml4_idx] =
      (__force uint64_t)pdpt_phys | PTE_PRESENT | PTE_RW | PTE_USER;
  return pdpt;
}

// Ensure a PD entry exists for the given virtual address.
// Returns the virtual address of the PT.
__attribute__((no_sanitize("kernel-address"))) uint64_t *
ensure_pt_in_pd(uint64_t *pd_or_pdpt, uint64_t vaddr, int level) {
  // level 2 = PDPT (need PD), level 1 = PD (need PT)
  uint64_t idx;
  if (level == 2) {
    idx = (vaddr >> 30) & 0x1FF;
  } else {
    idx = (vaddr >> 21) & 0x1FF;
  }
  if (pd_or_pdpt[idx] & 0x01) {
    return (__force uint64_t *)phys_to_virt(
        (__force phys_addr_t)(pd_or_pdpt[idx] & 0x000FFFFFFFFFF000ULL));
  }
  // Allocate next-level table
  struct page *table_page = bfc_alloc_page(1);
  if (!table_page)
    return NULL;
  uint64_t table_phys = (__force uint64_t)page_to_phys(table_page);
  uint64_t table_virt =
      (__force uint64_t)phys_to_virt((__force phys_addr_t)table_phys);
  uint64_t *table = (uint64_t *)table_virt;
  for (int i = 0; i < 512; i++) {
    table[i] = 0;
  }
  pd_or_pdpt[idx] =
      (__force uint64_t)table_phys | PTE_PRESENT | PTE_RW | PTE_USER;
  return table;
}

__attribute__((no_sanitize("kernel-address"))) bool
map_user_page_direct(uint64_t *new_pml4, uint64_t vaddr, uint64_t phys,
                     uint64_t flags) {
  uint64_t *pdpt = ensure_pd(new_pml4, vaddr);
  if (!pdpt)
    return false;
  uint64_t *pd = ensure_pt_in_pd(pdpt, vaddr, 2);
  if (!pd)
    return false;
  uint64_t *pt = ensure_pt_in_pd(pd, vaddr, 1);
  if (!pt)
    return false;

  uint64_t pt_idx = (vaddr >> 12) & 0x1FF;
  // Refuse to overwrite an existing mapping (prevents silent SHM/mmap
  // corruption)
  if (pt[pt_idx] & PTE_PRESENT)
    return false;
  pt[pt_idx] = phys | flags;
  return true;
}

__attribute__((no_sanitize("kernel-address"))) bool
map_user_pages(uint64_t *pml4, uint64_t vaddr_start, uint64_t vaddr_end,
               uint64_t flags, int *pages_mapped) {
  *pages_mapped = 0;
  uint64_t vaddr = ALIGN_UP(vaddr_start, PAGE_SIZE);
  while (vaddr < vaddr_end) {
    // Skip already-mapped pages
    uint64_t *pdpt = ensure_pd(pml4, vaddr);
    if (!pdpt)
      return false;
    uint64_t *pd = ensure_pt_in_pd(pdpt, vaddr, 2);
    if (!pd)
      return false;
    uint64_t *pt = ensure_pt_in_pd(pd, vaddr, 1);
    if (!pt)
      return false;
    uint64_t pt_idx = (vaddr >> 12) & 0x1FF;
    if (pt[pt_idx] & PTE_PRESENT) {
      vaddr += PAGE_SIZE;
      continue;
    }

    struct page *page = bfc_alloc_page(1);
    if (!page)
      return false;
    uint64_t phys = (__force uint64_t)page_to_phys(page);

    // Zero the page
    uint8_t *dst = (__force uint8_t *)phys_to_virt((__force phys_addr_t)phys);
    for (size_t i = 0; i < PAGE_SIZE; i++)
      dst[i] = 0;

    pt[pt_idx] = phys | flags;
    (*pages_mapped)++;
    vaddr += PAGE_SIZE;
  }
  return true;
}

__attribute__((no_sanitize("kernel-address"))) void
unmap_user_pages(uint64_t *pml4, uint64_t vaddr_start, uint64_t vaddr_end,
                 int count) {
  uint64_t vaddr = ALIGN_UP(vaddr_start, PAGE_SIZE);
  int freed = 0;
  while (vaddr < vaddr_end && freed < count) {
    uint64_t *pdpt = ensure_pd(pml4, vaddr);
    if (!pdpt)
      return;
    uint64_t *pd = ensure_pt_in_pd(pdpt, vaddr, 2);
    if (!pd)
      return;
    uint64_t *pt = ensure_pt_in_pd(pd, vaddr, 1);
    if (!pt)
      return;

    uint64_t pt_idx = (vaddr >> 12) & 0x1FF;
    if (pt[pt_idx] & PTE_PRESENT) {
      uint64_t phys = pt[pt_idx] & PTE_PHYS_MASK;
      struct page *p = &bfc_frames[PHY_TO_PAGE(phys)];
      if (refcount_dec_and_test(&p->p_refcount)) {
        bfc_free_page(p, 1);
      }
      pt[pt_idx] = 0;
      freed++;
    }
    vaddr += PAGE_SIZE;
  }
}

// Deep-copy user page tables from src_pml4 to dst_pml4.
// dst_pml4 must already have kernel entries copied.
// For each present user PTE, allocates new physical page and memcpys content.
// SHM/MAP_PHYSICAL pages: shares physical page (NO shm_get here — ref bump done
// in copy_mmap_regions). Sig trampoline page: shares physical page (no ref
// bump, global). Returns: 0 on success, negative errno on failure (dst
// partially filled).
__attribute__((no_sanitize("kernel-address"))) int
copy_page_table(uint64_t *src_pml4, uint64_t *dst_pml4,
                mmap_region *mmap_regions) {
  for (int pml4_idx = 0; pml4_idx < 256; pml4_idx++) {
    if (!(src_pml4[pml4_idx] & PTE_PRESENT))
      continue;

    uint64_t pdpt_phys = src_pml4[pml4_idx] & 0x000FFFFFFFFFF000ULL;
    uint64_t *src_pdpt =
        (uint64_t *)phys_to_virt((__force phys_addr_t)pdpt_phys);

    // Allocate new PDPT
    struct page *new_pdpt_page = bfc_alloc_page(1);
    if (!new_pdpt_page)
      return -ENOMEM;
    uint64_t new_pdpt_phys = (__force uint64_t)page_to_phys(new_pdpt_page);
    uint64_t *dst_pdpt =
        (uint64_t *)phys_to_virt((__force phys_addr_t)new_pdpt_phys);
    for (int i = 0; i < 512; i++)
      dst_pdpt[i] = 0;

    dst_pml4[pml4_idx] = new_pdpt_phys | (src_pml4[pml4_idx] & 0xFFF);

    for (int pdpt_idx = 0; pdpt_idx < 512; pdpt_idx++) {
      if (!(src_pdpt[pdpt_idx] & PTE_PRESENT))
        continue;
      if (src_pdpt[pdpt_idx] & PTE_PS) {
        dst_pdpt[pdpt_idx] = src_pdpt[pdpt_idx]; // huge page: share
        continue;
      }

      uint64_t pd_phys = src_pdpt[pdpt_idx] & 0x000FFFFFFFFFF000ULL;
      uint64_t *src_pd = (uint64_t *)phys_to_virt((__force phys_addr_t)pd_phys);

      struct page *new_pd_page = bfc_alloc_page(1);
      if (!new_pd_page)
        return -ENOMEM;
      uint64_t new_pd_phys = (__force uint64_t)page_to_phys(new_pd_page);
      uint64_t *dst_pd =
          (uint64_t *)phys_to_virt((__force phys_addr_t)new_pd_phys);
      for (int i = 0; i < 512; i++)
        dst_pd[i] = 0;

      dst_pdpt[pdpt_idx] = new_pd_phys | (src_pdpt[pdpt_idx] & 0xFFF);

      for (int pd_idx = 0; pd_idx < 512; pd_idx++) {
        if (!(src_pd[pd_idx] & PTE_PRESENT))
          continue;
        if (src_pd[pd_idx] & PTE_PS) {
          dst_pd[pd_idx] = src_pd[pd_idx];
          continue;
        }

        uint64_t pt_phys = src_pd[pd_idx] & 0x000FFFFFFFFFF000ULL;
        uint64_t *src_pt =
            (uint64_t *)phys_to_virt((__force phys_addr_t)pt_phys);

        struct page *new_pt_page = bfc_alloc_page(1);
        if (!new_pt_page)
          return -ENOMEM;
        uint64_t new_pt_phys = (__force uint64_t)page_to_phys(new_pt_page);
        uint64_t *dst_pt =
            (uint64_t *)phys_to_virt((__force phys_addr_t)new_pt_phys);
        for (int i = 0; i < 512; i++)
          dst_pt[i] = 0;

        dst_pd[pd_idx] = new_pt_phys | (src_pd[pd_idx] & 0xFFF);

        for (int pt_idx = 0; pt_idx < 512; pt_idx++) {
          uint64_t pte = src_pt[pt_idx];
          if (!(pte & PTE_PRESENT))
            continue;

          uint64_t leaf_phys = pte & PTE_PHYS_MASK;

          // Skip sig trampoline — shared physical page (global, no refcount
          // bump)
          if (sig_trampoline_phys != 0 && leaf_phys == sig_trampoline_phys) {
            dst_pt[pt_idx] = pte; // share
            continue;
          }

          // Check if this is an SHM/MAP_PHYSICAL page
          bool is_shared = false;
          for (mmap_region *mr = mmap_regions; mr; mr = mr->next) {
            if (mr->shm_obj != NULL) {
              shm *s = mr->shm_obj;
              if (s->page_list) {
                for (int pi = 0; pi < s->num_pages; pi++) {
                  if (leaf_phys == s->page_list[pi]) {
                    is_shared = true;
                    break;
                  }
                }
              } else if (s->phys != 0 && s->npages > 0) {
                if (leaf_phys >= s->phys &&
                    leaf_phys < s->phys + s->npages * PAGE_SIZE) {
                  is_shared = true;
                }
              }
            }
            if (mr->phys != 0 && leaf_phys >= mr->phys &&
                leaf_phys < mr->phys + mr->size) {
              is_shared = true;
            }
            if (is_shared)
              break;
          }

          if (is_shared) {
            dst_pt[pt_idx] = pte; // share PTE unchanged
            continue;
          }

          // COW: mark writable pages as read-only with COW flag
          struct page *phys_page = &bfc_frames[PHY_TO_PAGE(leaf_phys)];
          refcount_inc(&phys_page->p_refcount);

          if (pte & PTE_RW) {
            // Writable page: both parent and child get COW|~RW
            dst_pt[pt_idx] = leaf_phys | PTE_PRESENT | PTE_USER | PTE_COW |
                             (pte & PTE_NX) | (pte & PTE_ACCESSED);
            // Also modify parent PTE: clear RW, set COW
            src_pt[pt_idx] = leaf_phys | PTE_PRESENT | PTE_USER | PTE_COW |
                             (pte & PTE_NX) | (pte & PTE_ACCESSED);
          } else {
            // Read-only/execute-only page: share directly (already
            // non-writable)
            dst_pt[pt_idx] = pte; // copy PTE as-is
          }
        }
      }
    }
  }
  return 0;
}

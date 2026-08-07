/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/bsd/dev_mmap.h"

#include "arch/x64/paging.h"
#include "arch/x64/utils.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/mem/vma.h"
#include "kernel/xcore/sparse.h"
#include "utils/macro.h"
#include <stddef.h>
#include <xos/errno.h>
#include <xos/mman.h>
#include <xos/page.h>

void dev_mmap_abort(struct dev_mmap_backing *backing) {
  if (!backing || !backing->owner)
    return;
  if (backing->owner_ops && backing->owner_ops->put)
    backing->owner_ops->put(backing->owner);
  backing->owner = NULL;
}

int64_t dev_mmap_commit(mm *address_space, uint64_t *pml4,
                        const struct dev_mmap_request *request,
                        struct dev_mmap_backing *backing) {
  if (!address_space || !pml4 || !request || !backing || !backing->owner ||
      !backing->owner_ops || !backing->owner_ops->get ||
      !backing->owner_ops->put || !backing->pages || !request->length ||
      (request->length & (PAGE_SIZE - 1)) ||
      backing->page_count != request->length / PAGE_SIZE ||
      (backing->cache_flags & ~DEV_MMAP_CACHE_UC))
    return -EINVAL;

  int64_t picked = vma_pick_addr(
      address_space, pml4, request->addr, request->length, request->flags,
      request->addr ? ALIGN_DOWN(request->addr, PAGE_SIZE) : 0);
  if (picked < 0)
    return picked;

  uint64_t vaddr = (uint64_t)picked;
  uint64_t pte_flags = PTE_PRESENT | PTE_USER;
  if (request->prot & PROT_WRITE)
    pte_flags |= PTE_RW;
  if (!(request->prot & PROT_EXEC))
    pte_flags |= PTE_NX;
  if (backing->cache_flags & DEV_MMAP_CACHE_UC)
    pte_flags |= PTE_PCD | PTE_PWT;

  size_t mapped = 0;
  for (; mapped < backing->page_count; mapped++) {
    if (!backing->pages[mapped] ||
        !map_user_page_direct(
            pml4, vaddr + mapped * PAGE_SIZE,
            (__force uint64_t)page_to_phys(backing->pages[mapped]), pte_flags))
      break;
  }
  if (mapped != backing->page_count) {
    for (size_t i = 0; i < mapped; i++)
      clear_user_pte(pml4, vaddr + i * PAGE_SIZE);
    return -ENOMEM;
  }

  mmap_region *region = kmalloc(sizeof(*region));
  if (!region) {
    for (size_t i = 0; i < mapped; i++)
      clear_user_pte(pml4, vaddr + i * PAGE_SIZE);
    return -ENOMEM;
  }
  __memset(region, 0, sizeof(*region));
  region->vaddr = vaddr;
  region->size = request->length;
  region->prot = request->prot;
  region->fd = -1;
  region->offset = request->offset;
  region->flags = request->flags;
  if (backing->cache_flags & DEV_MMAP_CACHE_UC)
    region->flags |= KMAP_UC;
  if (vma_adopt_owner(region, backing->owner, backing->owner_ops) != 0 ||
      vma_insert_sorted(address_space, region) != 0) {
    for (size_t i = 0; i < mapped; i++)
      clear_user_pte(pml4, vaddr + i * PAGE_SIZE);
    // The caller still owns backing's reference on commit failure.
    region->flags &= ~KMAP_VMA_OWNER;
    kfree(region);
    return -ENOMEM;
  }
  backing->owner = NULL;
  if (!(request->flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) &&
      (request->addr == 0 || vaddr == address_space->mmap_brk))
    address_space->mmap_brk = vaddr + request->length;
  return picked;
}

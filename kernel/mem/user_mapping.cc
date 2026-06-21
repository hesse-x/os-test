// User-space page mapping functions
// Used by proc.cc (process creation) and trap.cc (sys_mmap/sys_munmap/sys_spawn)

#include "kernel/mem/alloc.h"
#include "kernel/proc.h"
#include "arch/x64/paging.h"
#include "common/macro.h"

// Ensure a PDPT entry exists for the given virtual address in user PML4.
// Returns the virtual address of the PD, or allocates a new one.
uint64_t *ensure_pd(uint64_t *new_pml4, uint64_t vaddr) {
    uint64_t pml4_idx = (vaddr >> 39) & 0x1FF;
    if (new_pml4[pml4_idx] & 0x01) {
        return (uint64_t *)phys_to_virt(new_pml4[pml4_idx] & 0x000FFFFFFFFFF000ULL);
    }
    // Allocate new PDPT
    Page *pdpt_page = bfc_alloc.alloc_page(1);
    if (!pdpt_page) return nullptr;
    uint64_t pdpt_phys = page_to_phys(pdpt_page);
    uint64_t pdpt_virt = phys_to_virt(pdpt_phys);
    uint64_t *pdpt = (uint64_t *)pdpt_virt;
    for (int i = 0; i < 512; i++) {
        pdpt[i] = 0;
    }
    new_pml4[pml4_idx] = pdpt_phys | PTE_PRESENT | PTE_RW | PTE_USER;
    return pdpt;
}

// Ensure a PD entry exists for the given virtual address.
// Returns the virtual address of the PT.
uint64_t *ensure_pt_in_pd(uint64_t *pd_or_pdpt, uint64_t vaddr, int level) {
    // level 2 = PDPT (need PD), level 1 = PD (need PT)
    uint64_t idx;
    if (level == 2) {
        idx = (vaddr >> 30) & 0x1FF;
    } else {
        idx = (vaddr >> 21) & 0x1FF;
    }
    if (pd_or_pdpt[idx] & 0x01) {
        return (uint64_t *)phys_to_virt(pd_or_pdpt[idx] & 0x000FFFFFFFFFF000ULL);
    }
    // Allocate next-level table
    Page *table_page = bfc_alloc.alloc_page(1);
    if (!table_page) return nullptr;
    uint64_t table_phys = page_to_phys(table_page);
    uint64_t table_virt = phys_to_virt(table_phys);
    uint64_t *table = (uint64_t *)table_virt;
    for (int i = 0; i < 512; i++) {
        table[i] = 0;
    }
    pd_or_pdpt[idx] = table_phys | PTE_PRESENT | PTE_RW | PTE_USER;
    return table;
}

bool map_user_page_direct(uint64_t *new_pml4, uint64_t vaddr, uint64_t phys,
                          uint64_t flags) {
    uint64_t *pdpt = ensure_pd(new_pml4, vaddr);
    if (!pdpt) return false;
    uint64_t *pd = ensure_pt_in_pd(pdpt, vaddr, 2);
    if (!pd) return false;
    uint64_t *pt = ensure_pt_in_pd(pd, vaddr, 1);
    if (!pt) return false;

    uint64_t pt_idx = (vaddr >> 12) & 0x1FF;
    // Refuse to overwrite an existing mapping (prevents silent SHM/mmap corruption)
    if (pt[pt_idx] & PTE_PRESENT) return false;
    pt[pt_idx] = phys | flags;
    return true;
}

bool map_user_pages(uint64_t *pml4, uint64_t vaddr_start, uint64_t vaddr_end,
                    uint64_t flags, int *pages_mapped) {
    *pages_mapped = 0;
    uint64_t vaddr = ALIGN_UP(vaddr_start, PAGE_SIZE);
    while (vaddr < vaddr_end) {
        // Skip already-mapped pages
        uint64_t *pdpt = ensure_pd(pml4, vaddr);
        if (!pdpt) return false;
        uint64_t *pd = ensure_pt_in_pd(pdpt, vaddr, 2);
        if (!pd) return false;
        uint64_t *pt = ensure_pt_in_pd(pd, vaddr, 1);
        if (!pt) return false;
        uint64_t pt_idx = (vaddr >> 12) & 0x1FF;
        if (pt[pt_idx] & PTE_PRESENT) {
            vaddr += PAGE_SIZE;
            continue;
        }

        Page *page = bfc_alloc.alloc_page(1);
        if (!page) return false;
        uint64_t phys = page_to_phys(page);

        // Zero the page
        uint8_t *dst = (uint8_t *)phys_to_virt(phys);
        for (size_t i = 0; i < PAGE_SIZE; i++) dst[i] = 0;

        pt[pt_idx] = phys | flags;
        (*pages_mapped)++;
        vaddr += PAGE_SIZE;
    }
    return true;
}

void unmap_user_pages(uint64_t *pml4, uint64_t vaddr_start, uint64_t vaddr_end,
                      int count) {
    uint64_t vaddr = ALIGN_UP(vaddr_start, PAGE_SIZE);
    int freed = 0;
    while (vaddr < vaddr_end && freed < count) {
        uint64_t *pdpt = ensure_pd(pml4, vaddr);
        if (!pdpt) return;
        uint64_t *pd = ensure_pt_in_pd(pdpt, vaddr, 2);
        if (!pd) return;
        uint64_t *pt = ensure_pt_in_pd(pd, vaddr, 1);
        if (!pt) return;

        uint64_t pt_idx = (vaddr >> 12) & 0x1FF;
        if (pt[pt_idx] & PTE_PRESENT) {
            uint64_t phys = pt[pt_idx] & 0x000FFFFFFFFFF000ULL;
            Page *p = &BFCAllocator::frames[PHY_TO_PAGE(phys)];
            bfc_alloc.free_page(p, 1);
            pt[pt_idx] = 0;
            freed++;
        }
        vaddr += PAGE_SIZE;
    }
}

// Check if a physical address belongs to a shared/MAP_PHYSICAL region
static bool is_shared_page(uint64_t leaf_phys, mm_t *src_mm) {
    for (mmap_region *mr = src_mm->mmap_regions; mr; mr = mr->next) {
        if (mr->shm_obj != nullptr) {
            struct shm *s = mr->shm_obj;
            if (s->page_list) {
                for (int pi = 0; pi < s->num_pages; pi++) {
                    if (leaf_phys == s->page_list[pi]) return true;
                }
            } else if (s->phys != 0 && s->npages > 0) {
                if (leaf_phys >= s->phys && leaf_phys < s->phys + s->npages * PAGE_SIZE)
                    return true;
            }
        }
        if (mr->phys != 0 && leaf_phys >= mr->phys &&
            leaf_phys < mr->phys + mr->size) {
            return true;
        }
    }
    return false;
}

// Deep-copy user page tables for fork.
// Walks src PML4[0-255], copies leaf pages, shares SHM/MAP_PHYSICAL pages.
// Returns new PML4 physical address, or 0 on failure.
uint64_t copy_page_table(uint64_t src_pml4_phys, mm_t *src_mm) {
    // Allocate new PML4
    Page *pml4_page = bfc_alloc.alloc_page(1);
    if (!pml4_page) return 0;
    uint64_t dst_pml4_phys = page_to_phys(pml4_page);
    uint64_t *dst_pml4 = (uint64_t *)phys_to_virt(dst_pml4_phys);
    uint64_t *src_pml4 = (uint64_t *)phys_to_virt(src_pml4_phys);

    // Zero and copy kernel entry
    for (int i = 0; i < 512; i++) dst_pml4[i] = 0;
    dst_pml4[511] = src_pml4[511];

    // Walk user entries [0-255]
    for (int pml4_idx = 0; pml4_idx < 256; pml4_idx++) {
        if (!(src_pml4[pml4_idx] & PTE_PRESENT)) continue;

        uint64_t *src_pdpt = (uint64_t *)phys_to_virt(
            src_pml4[pml4_idx] & 0x000FFFFFFFFFF000ULL);

        for (int pdpt_idx = 0; pdpt_idx < 512; pdpt_idx++) {
            if (!(src_pdpt[pdpt_idx] & PTE_PRESENT)) continue;
            if (src_pdpt[pdpt_idx] & PTE_PS) continue;

            uint64_t *src_pd = (uint64_t *)phys_to_virt(
                src_pdpt[pdpt_idx] & 0x000FFFFFFFFFF000ULL);

            for (int pd_idx = 0; pd_idx < 512; pd_idx++) {
                if (!(src_pd[pd_idx] & PTE_PRESENT)) continue;
                if (src_pd[pd_idx] & PTE_PS) continue;

                uint64_t *src_pt = (uint64_t *)phys_to_virt(
                    src_pd[pd_idx] & 0x000FFFFFFFFFF000ULL);

                for (int pt_idx = 0; pt_idx < 512; pt_idx++) {
                    uint64_t pte = src_pt[pt_idx];
                    if (!(pte & PTE_PRESENT)) continue;

                    uint64_t leaf_phys = pte & 0x000FFFFFFFFFF000ULL;
                    uint64_t flags = pte & (~0x000FFFFFFFFFF000ULL);  // keep permission flags

                    // Compute vaddr for mapping
                    uint64_t vaddr = ((uint64_t)pml4_idx << 39) |
                                     ((uint64_t)pdpt_idx << 30) |
                                     ((uint64_t)pd_idx << 21) |
                                     ((uint64_t)pt_idx << 12);

                    if (is_shared_page(leaf_phys, src_mm)) {
                        // SHM/MAP_PHYSICAL: map same physical page
                        if (!map_user_page_direct(dst_pml4, vaddr, leaf_phys, flags))
                            return 0;
                    } else {
                        // Private page: allocate new page, copy content
                        Page *new_page = bfc_alloc.alloc_page(1);
                        if (!new_page) return 0;
                        uint64_t new_phys = page_to_phys(new_page);
                        uint8_t *src_addr = (uint8_t *)phys_to_virt(leaf_phys);
                        uint8_t *dst_addr = (uint8_t *)phys_to_virt(new_phys);
                        for (size_t i = 0; i < PAGE_SIZE; i++)
                            dst_addr[i] = src_addr[i];
                        if (!map_user_page_direct(dst_pml4, vaddr, new_phys, flags))
                            return 0;
                    }
                }
            }
        }
    }
    return dst_pml4_phys;
}

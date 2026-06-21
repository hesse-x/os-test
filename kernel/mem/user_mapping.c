// User-space page mapping functions
// Used by proc.c (process creation) and trap.c (sys_mmap/sys_munmap/sys_spawn)

#include <stdbool.h>
#include "kernel/mem/alloc.h"
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
    Page *pdpt_page = bfc_alloc_page(1);
    if (!pdpt_page) return NULL;
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
    Page *table_page = bfc_alloc_page(1);
    if (!table_page) return NULL;
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

        Page *page = bfc_alloc_page(1);
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
            Page *p = &bfc_frames[PHY_TO_PAGE(phys)];
            bfc_free_page(p, 1);
            pt[pt_idx] = 0;
            freed++;
        }
        vaddr += PAGE_SIZE;
    }
}

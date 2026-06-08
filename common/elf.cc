#include "common/elf.h"
#include "kernel/mem/alloc.h"
#include "kernel/proc.h"
#include "arch/x64/paging.h"
#include "common/macro.h"
#include <stddef.h>

static uint64_t page_to_phys(Page *p) {
    return (uint64_t)(p - BFCAllocator::frames) * PAGE_SIZE;
}

static uint64_t phys_to_virt(uint64_t phys) {
    return phys + VMA_BASE;
}

// Ensure a PDPT entry exists for the given virtual address in new_pml4.
static uint64_t *ensure_pd(uint64_t *new_pml4, uint64_t vaddr) {
    uint64_t pml4_idx = (vaddr >> 39) & 0x1FF;
    if (new_pml4[pml4_idx] & 0x01) {
        return (uint64_t *)phys_to_virt(new_pml4[pml4_idx] & ~0xFFF);
    }
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
static uint64_t *ensure_pt_in_pd(uint64_t *pd_or_pdpt, uint64_t vaddr, int level) {
    uint64_t idx;
    if (level == 2) {
        idx = (vaddr >> 30) & 0x1FF;
    } else {
        idx = (vaddr >> 21) & 0x1FF;
    }
    if (pd_or_pdpt[idx] & 0x01) {
        return (uint64_t *)phys_to_virt(pd_or_pdpt[idx] & ~0xFFF);
    }
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

// Map a single 4KB page at vaddr into new_pml4, copying data from src.
static bool map_page(uint64_t *new_pml4, uint64_t vaddr, const uint8_t *src,
                     uint64_t copy_len) {
    Page *page = bfc_alloc.alloc_page(1);
    if (!page) return false;
    uint64_t page_phys = page_to_phys(page);
    uint64_t page_virt = phys_to_virt(page_phys);

    // Clear page first (handles BSS zeroing)
    uint8_t *dst = (uint8_t *)page_virt;
    for (size_t i = 0; i < PAGE_SIZE; i++) {
        dst[i] = 0;
    }

    // Copy file data
    if (src && copy_len > 0) {
        for (uint64_t i = 0; i < copy_len; i++) {
            dst[i] = src[i];
        }
    }

    // Walk: PML4 → PDPT → PD → PT
    uint64_t *pdpt = ensure_pd(new_pml4, vaddr);
    if (!pdpt) return false;
    uint64_t *pd = ensure_pt_in_pd(pdpt, vaddr, 2);
    if (!pd) return false;
    uint64_t *pt = ensure_pt_in_pd(pd, vaddr, 1);
    if (!pt) return false;

    uint64_t pt_idx = (vaddr >> 12) & 0x1FF;
    pt[pt_idx] = page_phys | PTE_PRESENT | PTE_RW | PTE_USER;

    return true;
}

elf_load_result elf_load(const uint8_t *data, uint64_t size,
                         uint64_t *new_pml4) {
    elf_load_result result = {0, false};

    // 1. Validate ELF magic
    if (size < sizeof(Elf64_Ehdr)) return result;
    if (data[0] != 0x7F || data[1] != 'E' ||
        data[2] != 'L'  || data[3] != 'F')
        return result;

    // Check ELF class (should be 64-bit)
    if (data[4] != 2) return result;  // EI_CLASS != ELFCLASS64

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)data;
    result.entry = ehdr->e_entry;

    // 2. Iterate program headers
    for (int i = 0; i < ehdr->e_phnum; i++) {
        uint64_t ph_off = ehdr->e_phoff + i * ehdr->e_phentsize;
        if (ph_off + sizeof(Elf64_Phdr) > size) return result;

        Elf64_Phdr *ph = (Elf64_Phdr *)(data + ph_off);

        if (ph->p_type != PT_LOAD)
            continue;

        if (ph->p_memsz == 0)
            continue;

        // 3. Map pages covering this segment
        uint64_t first_page = ph->p_vaddr & ~0xFFFULL;
        uint64_t last_page = (ph->p_vaddr + ph->p_memsz - 1) & ~0xFFFULL;

        for (uint64_t page_addr = first_page; page_addr <= last_page;
             page_addr += PAGE_SIZE) {
            uint64_t file_start = ph->p_offset;
            uint64_t file_end = ph->p_offset + ph->p_filesz;

            uint64_t page_off = page_addr - ph->p_vaddr;

            const uint8_t *src = nullptr;
            uint64_t copy_len = 0;

            if (page_off < ph->p_filesz) {
                uint64_t page_file_start = page_off;
                uint64_t page_file_end = page_off + PAGE_SIZE;
                if (page_file_end > ph->p_filesz)
                    page_file_end = ph->p_filesz;
                copy_len = page_file_end - page_file_start;
                src = data + file_start + page_off;
            }

            if (!map_page(new_pml4, page_addr, src, copy_len))
                return result;
        }
    }

    result.success = true;
    return result;
}

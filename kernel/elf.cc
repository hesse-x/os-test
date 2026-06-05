#include "kernel/elf.h"
#include "kernel/mem/alloc.h"
#include "arch/x86/paging.h"
#include "common/macro.h"
#include <stddef.h>

static uint32_t page_to_phys(Page *p) {
    return (uint32_t)(p - BFCAllocator::frames) * PAGE_SIZE;
}

static uint32_t phys_to_virt(uint32_t phys) {
    return phys + VMA_BASE;
}

// Ensure a page table exists for the given virtual address in new_pd.
// Returns the virtual address of the PT, or allocates a new one.
static uint32_t *ensure_pt(uint32_t *new_pd, uint32_t vaddr) {
    uint32_t pd_idx = vaddr >> 22;
    if (new_pd[pd_idx] & 0x01) {
        // PT already exists
        return (uint32_t *)phys_to_virt(new_pd[pd_idx] & ~0xFFF);
    }
    // Allocate new PT
    Page *pt_page = bfc_alloc.alloc_page(1);
    if (!pt_page) return nullptr;
    uint32_t pt_phys = page_to_phys(pt_page);
    uint32_t pt_virt = phys_to_virt(pt_phys);
    uint32_t *pt = (uint32_t *)pt_virt;
    for (int i = 0; i < 1024; i++) {
        pt[i] = 0;
    }
    new_pd[pd_idx] = pt_phys | 0x07;  // Present + Writable + User
    return pt;
}

// Map a single 4KB page at vaddr into new_pd, copying data from src.
static bool map_page(uint32_t *new_pd, uint32_t vaddr, const uint8_t *src,
                     uint32_t copy_len) {
    Page *page = bfc_alloc.alloc_page(1);
    if (!page) return false;
    uint32_t page_phys = page_to_phys(page);
    uint32_t page_virt = phys_to_virt(page_phys);

    // Clear page first (handles BSS zeroing)
    uint8_t *dst = (uint8_t *)page_virt;
    for (int i = 0; i < PAGE_SIZE; i++) {
        dst[i] = 0;
    }

    // Copy file data
    if (src && copy_len > 0) {
        for (uint32_t i = 0; i < copy_len; i++) {
            dst[i] = src[i];
        }
    }

    // Install PTE
    uint32_t *pt = ensure_pt(new_pd, vaddr);
    if (!pt) return false;
    uint32_t pt_idx = (vaddr >> 12) & 0x3FF;
    pt[pt_idx] = page_phys | 0x07;  // Present + Writable + User

    return true;
}

elf_load_result elf_load(const uint8_t *data, uint32_t size,
                         uint32_t *new_pd) {
    elf_load_result result = {0, false};

    // 1. Validate ELF magic
    if (size < sizeof(Elf32_Ehdr)) return result;
    if (data[0] != 0x7F || data[1] != 'E' ||
        data[2] != 'L'  || data[3] != 'F')
        return result;

    Elf32_Ehdr *ehdr = (Elf32_Ehdr *)data;
    result.entry = ehdr->e_entry;

    // 2. Iterate program headers
    for (int i = 0; i < ehdr->e_phnum; i++) {
        uint32_t ph_off = ehdr->e_phoff + i * ehdr->e_phentsize;
        if (ph_off + sizeof(Elf32_Phdr) > size) return result;

        Elf32_Phdr *ph = (Elf32_Phdr *)(data + ph_off);

        if (ph->p_type != PT_LOAD)
            continue;

        if (ph->p_memsz == 0)
            continue;

        // 3. Map pages covering this segment
        uint32_t first_page = ph->p_vaddr & ~0xFFF;
        uint32_t last_page = (ph->p_vaddr + ph->p_memsz - 1) & ~0xFFF;

        for (uint32_t page_addr = first_page; page_addr <= last_page;
             page_addr += PAGE_SIZE) {
            // Compute what part of this page has file data
            uint32_t file_start = ph->p_offset;
            uint32_t file_end = ph->p_offset + ph->p_filesz;

            // Offset within this page
            uint32_t page_off = page_addr - ph->p_vaddr;

            // Source pointer and length for this page
            const uint8_t *src = nullptr;
            uint32_t copy_len = 0;

            // Intersection of [page_off, page_off+PAGE_SIZE) with
            // [0, p_filesz) offset by page_off
            if (page_off < ph->p_filesz) {
                uint32_t page_file_start = page_off;
                uint32_t page_file_end = page_off + PAGE_SIZE;
                if (page_file_end > ph->p_filesz)
                    page_file_end = ph->p_filesz;
                copy_len = page_file_end - page_file_start;
                src = data + file_start + page_off;
            }

            if (!map_page(new_pd, page_addr, src, copy_len))
                return result;
        }
    }

    result.success = true;
    return result;
}

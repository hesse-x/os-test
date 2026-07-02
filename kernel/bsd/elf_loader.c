#include "kernel/bsd/elf_loader.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/xtask.h"
#include "kernel/bsd/types.h"
#include "kernel/xcore/log.h"
#include "arch/x64/paging.h"
#include "common/macro.h"
#include <stddef.h>
#include <stdbool.h>

// page_to_phys, phys_to_virt — defined in kernel/mem/alloc.c
// ensure_pd, ensure_pt_in_pd, map_user_page_direct — defined in kernel/mem/user_mapping.c

// Map a single 4KB page at vaddr into new_pml4, copying data from src.
static bool map_page(uint64_t *new_pml4, uint64_t vaddr, const uint8_t *src,
                     uint64_t copy_len, uint64_t elf_flags) {
    Page *page = bfc_alloc_page(1);
    if (!page) return false;
    uint64_t page_phys = (__force uint64_t)page_to_phys(page);
    uint64_t page_virt = (__force uint64_t)phys_to_virt((__force phys_addr_t)page_phys);

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

    // Compute PTE flags from ELF segment flags
    uint64_t pte_flags = PTE_PRESENT | PTE_USER;
    if (elf_flags & PF_W) pte_flags |= PTE_RW;
    if (!(elf_flags & PF_X)) pte_flags |= PTE_NX;

    uint64_t pt_idx = (vaddr >> 12) & 0x1FF;
    pt[pt_idx] = page_phys | pte_flags;

    return true;
}

static elf_load_result_t elf_load_internal(const uint8_t *data, uint64_t size,
                                  uint64_t *new_pml4, uint64_t base) {
    elf_load_result_t result = {0};
    result.load_base = base;

    // 1. Validate ELF magic
    if (size < sizeof(Elf64_Ehdr)) return result;
    if (data[0] != 0x7F || data[1] != 'E' ||
        data[2] != 'L'  || data[3] != 'F')
        return result;

    // Check ELF class (should be 64-bit)
    if (data[4] != 2) return result;

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)data;
    result.entry = base + ehdr->e_entry;
    result.phnum = ehdr->e_phnum;
    result.phent = ehdr->e_phentsize;

    // 2. Iterate program headers
    bool phdr_found = false;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        uint64_t ph_off = ehdr->e_phoff + i * ehdr->e_phentsize;
        if (ph_off + sizeof(Elf64_Phdr) > size) return result;

        Elf64_Phdr *ph = (Elf64_Phdr *)(data + ph_off);

        if (ph->p_type == PT_TLS) {
            result.tls_tdata_size = ph->p_filesz;
            result.tls_tbss_size = ph->p_memsz - ph->p_filesz;
            result.tls_align = ph->p_align;
            result.tls_template_off = ph->p_offset;
            continue;
        }

        if (ph->p_type != PT_LOAD)
            continue;

        if (ph->p_memsz == 0)
            continue;

        // 记录第一个 PT_LOAD 用于 AT_PHDR：p_vaddr + e_phoff - p_offset
        // PHDR 表位于 ELF 文件 e_phoff 处，与首个 PT_LOAD 的文件偏移对齐
        if (!phdr_found) {
            result.phdr_vaddr = base + (ph->p_vaddr + ehdr->e_phoff - ph->p_offset);
            phdr_found = true;
        }

        // 3. Map pages covering this segment
        uint64_t first_page = base + (ph->p_vaddr & ~0xFFFULL);
        uint64_t last_page = base + ((ph->p_vaddr + ph->p_memsz - 1) & ~0xFFFULL);

        for (uint64_t page_addr = first_page; page_addr <= last_page;
             page_addr += PAGE_SIZE) {
            uint64_t file_start = ph->p_offset;

            uint64_t page_off = (page_addr - base) - ph->p_vaddr;

            const uint8_t *src = NULL;
            uint64_t copy_len = 0;

            if (page_off < ph->p_filesz) {
                uint64_t page_file_start = page_off;
                uint64_t page_file_end = page_off + PAGE_SIZE;
                if (page_file_end > ph->p_filesz)
                    page_file_end = ph->p_filesz;
                copy_len = page_file_end - page_file_start;
                src = data + file_start + page_off;
            }

            if (!map_page(new_pml4, page_addr, src, copy_len, ph->p_flags)) {
                printk(LOG_ERROR, "elf_load: map_page failed for vaddr=%lx\n", page_addr);
                return result;
            }
        }
    }

    result.success = true;

    return result;
}

elf_load_result_t elf_load(const uint8_t *data, uint64_t size,
                         uint64_t *new_pml4) {
    return elf_load_internal(data, size, new_pml4, 0);
}

elf_load_result_t elf_load_at(const uint8_t *data, uint64_t size,
                              uint64_t *new_pml4, uint64_t base) {
    return elf_load_internal(data, size, new_pml4, base);
}

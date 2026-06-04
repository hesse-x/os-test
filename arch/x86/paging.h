#ifndef ARCH_X86_PAGING_H
#define ARCH_X86_PAGING_H

#include <stddef.h>
#include <stdint.h>
#include "arch/x86/utils.h"
#include "common/macro.h"

// ===================== Constants =====================
#define PAGE_SIZE 4096
#define VMA_BASE 0xC0000000
#define KERNEL_LMA_BASE 0x100000
#define KERNEL_VMA_BASE 0xC0100000
#define PHY_ADDR(addr) ((uintptr_t)(addr) - VMA_BASE)
#define PTX_SHIFT 12
#define PHY_TO_PAGE(addr) (addr >> PTX_SHIFT)
#define GET_PAGE_NUM(len) (ALIGN_UP(len, PAGE_SIZE) / PAGE_SIZE)

// ===================== GDT =====================
typedef struct {
  uint16_t limit_low;
  uint16_t base_low;
  uint8_t base_middle;
  uint8_t access;
  uint8_t granularity;
  uint8_t base_high;
} __attribute__((packed)) gdt_entry_t;

typedef struct {
  uint16_t limit;
  uint32_t base;
} __attribute__((packed)) gdt_ptr_t;

// ===================== Global variables =====================
extern "C" {
extern uint32_t page_directory[1024];
extern uint32_t page_table[1024];
extern uintptr_t device_vma_base;

// ===================== Functions =====================
void enable_page();
void gdt_init();
void extend_mapping(uint64_t max_phys_addr);
void flush_tlb();
void *bump_alloc(size_t size);
void bump_init_phys(uintptr_t start);
}

#endif // ARCH_X86_PAGING_H

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

// ===================== TSS =====================
typedef struct {
  uint32_t link;
  uint32_t esp0;
  uint32_t ss0;
  uint32_t esp1;
  uint32_t ss1;
  uint32_t esp2;
  uint32_t ss2;
  uint32_t cr3;
  uint32_t eip;
  uint32_t eflags;
  uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
  uint32_t es, cs, ss, ds, fs, gs;
  uint32_t ldt;
  uint16_t trap;
  uint16_t iomap_base;
} __attribute__((packed)) tss_t;

#define USER_CS  0x1B    // index 3, RPL=3
#define USER_DS  0x23    // index 4, RPL=3
#define TSS_SEL  0x28    // index 5

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
void bump_disable();
}

#endif // ARCH_X86_PAGING_H

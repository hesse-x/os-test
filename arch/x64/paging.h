/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ARCH_X64_PAGING_H
#define ARCH_X64_PAGING_H

#include "boot/boot.h"
#include "kernel/xcore/sparse.h"
#include <stddef.h>
#include <stdint.h>

// ===================== Page table entry flags =====================
#define PTE_PRESENT (1ULL << 0)
#define PTE_RW (1ULL << 1)
#define PTE_USER (1ULL << 2)
#define PTE_PWT (1ULL << 3)      // Page-level Write-Through
#define PTE_PCD (1ULL << 4)      // Page-level Cache Disable
#define PTE_ACCESSED (1ULL << 5) // HW: set by CPU on any access
#define PTE_DIRTY (1ULL << 6)    // HW: set by CPU on write
#define PTE_PS (1ULL << 7)       // Page size (2MB huge page at PD level)
#define PTE_GLOBAL (1ULL << 8)   // Global page (not flushed on CR3 reload)
#define PTE_COW (1ULL << 9)      // SW: Copy-on-Write marker
#define PTE_NX (1ULL << 63)      // No-execute

#define PTE_PHYS_MASK 0x000FFFFFFFFFF000ULL // Extract physical address from PTE

// PAT index selection via PCD+PWT (for 2MB huge pages at PD level)
#define PTE_UC (PTE_PCD | PTE_PWT) // PAT index 3 = UC
#define PTE_WC (PTE_PCD)           // PAT index 2 = WC (after PAT MSR reprogram)
#define PTE_WT (PTE_PWT)           // PAT index 1 = WT
// PAT index 0 = WB (no PCD, no PWT) — default, no flag needed

// ===================== Constants =====================
#define PHY_ADDR(addr) ((__force phys_addr_t)((uintptr_t)(addr)-VMA_BASE))

// ===================== GDT =====================
// 64-bit GDT entry: 8 bytes
typedef struct gdt_entry {
  uint16_t limit_low;
  uint16_t base_low;
  uint8_t base_middle;
  uint8_t access;
  uint8_t granularity;
  uint8_t base_high;
} __attribute__((packed)) gdt_entry;

// 64-bit GDT pointer: 10 bytes (64-bit base)
typedef struct gdt_ptr {
  uint16_t limit;
  uint64_t base;
} __attribute__((packed)) gdt_ptr;

// ===================== TSS (64-bit, 128 bytes + 8KB IOPM) =====================
#define IOPM_SIZE 8192 // 8KB IOPM bitmap (65536 ports / 8 bits per byte)
struct tss_struct {
  uint32_t reserved0;
  uint64_t rsp0;
  uint64_t rsp1;
  uint64_t rsp2;
  uint64_t reserved1;
  uint64_t ist[7];
  uint64_t reserved2;
  uint16_t reserved3;
  uint16_t iomap_base;
  uint8_t iopm[IOPM_SIZE]; // I/O permission bitmap
} __attribute__((packed));

#define USER_CS 0x2B // index 5, RPL=3 (SYSRET64 CS = STAR[63:48]+16 | 3)
#define USER_DS 0x23 // index 4, RPL=3 (SYSRET SS = STAR[63:48]+8 | 3)
#define TSS_SEL 0x30 // index 6

// ===================== Global variables =====================
extern uint64_t pml4[512];
extern uint64_t pdpt_ident[512];
extern uint64_t pdpt_hh[512];
extern uint64_t page_dir[512];
extern uintptr_t device_vma_base;
extern boot_info g_boot_info;
extern const uint8_t stack_bottom[8192];

// ===================== Functions =====================
void enable_paging(boot_info *bi_phys);
void gdt_init();
void extend_mapping(uint64_t max_phys_addr);
void flush_tlb();
void *bump_alloc(size_t size);
void bump_init_phys(uintptr_t start);
void bump_disable();
uintptr_t bump_end_phys();
void enable_nx();
void enable_sse();
void pat_init();
void log_cpu_caps(const char *tag);

#endif // ARCH_X64_PAGING_H

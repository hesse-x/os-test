#ifndef ARCH_X64_PAGING_H
#define ARCH_X64_PAGING_H

#include <stddef.h>
#include <stdint.h>
#include "arch/x64/utils.h"
#include "common/macro.h"
#include "arch/x64/memlayout.h"

// ===================== Page table entry flags =====================
#define PTE_PRESENT  (1ULL << 0)
#define PTE_RW       (1ULL << 1)
#define PTE_USER     (1ULL << 2)
#define PTE_PS       (1ULL << 7)   // Page size (2MB huge page at PD level)
#define PTE_NX       (1ULL << 63)  // No-execute

// ===================== Constants =====================
#define VMA_BASE 0xFFFFFFFF80000000ULL
#define KERNEL_LMA_BASE 0x100000
#define KERNEL_VMA_BASE 0xFFFFFFFF80100000ULL
#define PHY_ADDR(addr) ((uintptr_t)(addr) - VMA_BASE)

// ===================== boot_info (stub → kernel) =====================
#define BOOT_INFO_MAGIC 0x4F53424F544F4F42ULL  // "BOOTBOOS"

struct boot_info {
  uint64_t magic;
  uint64_t kernel_phys;     // kernel physical load address
  uint64_t rsdp;            // RSDP physical address
  uint64_t fb_addr;         // framebuffer physical address
  uint32_t fb_width;
  uint32_t fb_height;
  uint32_t fb_pitch;
  uint32_t fb_bpp;
  uint32_t fb_pixel_format; // 0=RGB, 1=BGR
  uint64_t mmap_addr;       // EFI memory descriptor array (physical)
  uint64_t mmap_size;       // total bytes
  uint64_t mmap_desc_size;  // single descriptor size
  uint64_t mmap_desc_ver;   // descriptor version
  // APIC info (from MADT)
  uint64_t lapic_base;      // LAPIC MMIO base physical address
  uint64_t ioapic_base;     // I/O APIC MMIO base physical address
  uint32_t ncpus;           // number of CPUs
  uint32_t apic_ids[4];     // APIC IDs (MAX_CPUS=4)
};

// ===================== GDT =====================
// 64位 GDT entry: 8字节
typedef struct {
  uint16_t limit_low;
  uint16_t base_low;
  uint8_t base_middle;
  uint8_t access;
  uint8_t granularity;
  uint8_t base_high;
} __attribute__((packed)) gdt_entry_t;

// 64位 GDT 指针: 10字节 (64-bit base)
typedef struct {
  uint16_t limit;
  uint64_t base;
} __attribute__((packed)) gdt_ptr_t;

// ===================== TSS (64-bit, 128字节 + 8KB IOPM) =====================
#define IOPM_SIZE 8192  // 8KB IOPM bitmap (65536 ports / 8 bits per byte)
typedef struct {
  uint32_t reserved0;
  uint64_t rsp0;
  uint64_t rsp1;
  uint64_t rsp2;
  uint64_t reserved1;
  uint64_t ist[7];
  uint64_t reserved2;
  uint16_t reserved3;
  uint16_t iomap_base;
  uint8_t  iopm[IOPM_SIZE];  // I/O permission bitmap
} __attribute__((packed)) tss_t;

#define USER_CS  0x2B    // index 5, RPL=3 (SYSRET64 CS = STAR[63:48]+16 | 3)
#define USER_DS  0x23    // index 4, RPL=3 (SYSRET SS = STAR[63:48]+8 | 3)
#define TSS_SEL  0x30    // index 6

// ===================== Global variables =====================
extern "C" {
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
}

#endif // ARCH_X64_PAGING_H

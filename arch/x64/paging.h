#ifndef ARCH_X64_PAGING_H
#define ARCH_X64_PAGING_H

#include <stddef.h>
#include <stdint.h>
#include "arch/x64/utils.h"
#include "common/macro.h"

// ===================== Constants =====================
#define PAGE_SIZE 4096
#define PAGE_SIZE_2M 0x200000
#define VMA_BASE 0xFFFFFFFF80000000ULL
#define KERNEL_LMA_BASE 0x100000
#define KERNEL_VMA_BASE 0xFFFFFFFF80100000ULL
#define PHY_ADDR(addr) ((uintptr_t)(addr) - VMA_BASE)
#define PTX_SHIFT 12
#define PHY_TO_PAGE(addr) ((addr) >> PTX_SHIFT)
#define GET_PAGE_NUM(len) (ALIGN_UP(len, PAGE_SIZE) / PAGE_SIZE)

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

// ===================== TSS (64-bit, 128字节) =====================
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
} __attribute__((packed)) tss_t;

#define USER_CS  0x1B    // index 3, RPL=3
#define USER_DS  0x23    // index 4, RPL=3
#define TSS_SEL  0x28    // index 5

// ===================== Global variables =====================
extern "C" {
extern uint64_t pml4[512];
extern uint64_t pdpt_ident[512];
extern uint64_t pdpt_hh[512];
extern uint64_t page_dir[512];
extern uintptr_t device_vma_base;
extern tss_t tss;
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
}

#endif // ARCH_X64_PAGING_H

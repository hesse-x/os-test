#ifndef MEMORY_H
#define MEMORY_H

#include "multiboot2.h"
#include "macro.h"
#include <stddef.h>
#include <stdint.h>

// ===================== Constants =====================
#define PAGE_SIZE 4096 // 4KB页帧大小
#define VMA_BASE 0xC0000000
#define KERNEL_LMA_BASE 0x100000
#define KERNEL_VMA_BASE 0xC0100000
#define PHY_ADDR(addr) ((uintptr_t)(addr) - VMA_BASE)
#define PTX_SHIFT 12
#define PHY_TO_PAGE(addr) (addr >> PTX_SHIFT)
#define GET_PAGE_NUM(len) (ALIGN_UP(len, PAGE_SIZE) / PAGE_SIZE)
#define KERNEL_CS 0x08
#define L16(x) ((uint16_t)((x) & 0xFFFF))
#define H16(x) ((uint16_t)(((x) >> 16) & 0xFFFF))

// ===================== GDT =====================

/* How every GDT entry is defined */
typedef struct {
  uint16_t limit_low;
  uint16_t base_low;
  uint8_t base_middle;
  uint8_t access;
  uint8_t granularity;
  uint8_t base_high;
} __attribute__((packed)) gdt_entry_t;

/* A pointer to the GDT array, for 'lgdt' */
typedef struct {
  uint16_t limit;
  uint32_t base;
} __attribute__((packed)) gdt_ptr_t;
extern "C" {
// ===================== 页帧描述符 =====================
// 页帧状态
enum class PageStatus : int8_t {
  FREE,    // 空闲
  USED,    // 已使用（如内核占用）
  RESERVED // 保留（硬件/BIOS占用）
};

// 单个页帧的描述符（记录状态）
struct Page {
  PageStatus status;
  size_t cont_page_num;
  Page *prev;
  Page *next;
};

struct BFCAllocator {
  void init();
  Page *alloc_page(size_t n);
  Page *free_page(Page *page, size_t n);
  size_t free_page_nums() const;

  static Page *frames;
  static Page *free_list;
};

// ===================== 全局变量声明 =====================
extern size_t total_page_frames;
extern uint32_t page_directory[1024];
extern uint32_t page_table[1024];
extern uintptr_t device_vma_base;

// ===================== GDT functions =====================
void set_gdt_gate(int n, uint32_t base, uint32_t limit, uint8_t access,
                  uint8_t gran);
void set_gdt();
void gdt_init();

// ===================== 函数声明 =====================
void enable_page();
void init_mem(uintptr_t mbi_addr);
void *bump_alloc(size_t size);
}
#endif // MEMORY_H

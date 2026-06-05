#ifndef KERNEL_MEM_ALLOC_H
#define KERNEL_MEM_ALLOC_H

#include <stdint.h>
#include <stddef.h>

// ===================== Page frame descriptor =====================
enum class PageStatus : int8_t {
  FREE,    // 空闲
  USED,    // 已使用（如内核占用）
  RESERVED // 保留（硬件/BIOS占用）
};

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

// ===================== Global variables =====================
extern "C" {
extern size_t total_page_frames;

void init_mem(uintptr_t mbi_addr);
}

extern BFCAllocator bfc_alloc;

#endif // KERNEL_MEM_ALLOC_H

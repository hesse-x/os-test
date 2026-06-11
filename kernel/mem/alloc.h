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

struct boot_info;

void init_mem(boot_info *bi);
}

extern BFCAllocator bfc_alloc;

// ===================== Address conversion =====================
uint64_t page_to_phys(Page *p);
uint64_t phys_to_virt(uint64_t phys);

// ===================== User page mapping =====================
uint64_t *ensure_pd(uint64_t *new_pml4, uint64_t vaddr);
uint64_t *ensure_pt_in_pd(uint64_t *pd_or_pdpt, uint64_t vaddr, int level);
bool map_user_page_direct(uint64_t *new_pml4, uint64_t vaddr, uint64_t phys,
                          uint64_t flags);
bool map_user_pages(uint64_t *pml4, uint64_t vaddr_start, uint64_t vaddr_end,
                    uint64_t flags, int *pages_mapped);
void unmap_user_pages(uint64_t *pml4, uint64_t vaddr_start, uint64_t vaddr_end,
                      int count);

#endif // KERNEL_MEM_ALLOC_H

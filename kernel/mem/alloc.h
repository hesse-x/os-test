#ifndef KERNEL_MEM_ALLOC_H
#define KERNEL_MEM_ALLOC_H

#include <stdint.h>
#include <stddef.h>
#include "kernel/spinlock.h"

#define NUM_KMALLOC_CLASSES 9

// ===================== Page frame descriptor =====================
enum class PageStatus : int8_t {
  FREE,    // 空闲
  USED,    // 已使用（如内核占用）
  SLAB,    // slab 页
  RESERVED // 保留（硬件/BIOS占用）
};

struct kmem_cache_t;

struct Page {
  PageStatus status;
  union {
    struct {
      size_t cont_page_num;   // 连续页数（BFC 空闲/已分配大块）
      Page *prev;             // BFC free list 双向链表
      Page *next;
    } bfc;

    struct {
      kmem_cache_t *cache;    // 所属 cache
      void *freelist;         // 空闲对象链表头（侵入式）
      uint32_t inuse;         // 已分配对象数
      uint32_t obj_count;     // 页内总对象数
      int8_t cpu_id;          // 正在使用此页的 CPU（-1 = 无）
      Page *partial_next;     // partial list 链接
      Page *partial_prev;
    } slab;
  };
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
extern spinlock_t bfc_lock;

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

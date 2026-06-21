#ifndef KERNEL_MEM_ALLOC_H
#define KERNEL_MEM_ALLOC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "kernel/spinlock.h"

#define NUM_KMALLOC_CLASSES 9

// ===================== Page frame descriptor =====================
typedef enum {
  PAGE_FREE,    // 空闲
  PAGE_USED,    // 已使用（如内核占用）
  PAGE_SLAB,    // slab 页
  PAGE_RESERVED // 保留（硬件/BIOS占用）
} page_status_t;

struct kmem_cache_t;

typedef struct Page {
  page_status_t status;
  union {
    struct {
      size_t cont_page_num;   // 连续页数（BFC 空闲/已分配大块）
      struct Page *prev;      // BFC free list 双向链表
      struct Page *next;
    } bfc;

    struct {
      struct kmem_cache_t *cache;    // 所属 cache
      void *freelist;         // 空闲对象链表头（侵入式）
      uint32_t inuse;         // 已分配对象数
      uint32_t obj_count;     // 页内总对象数
      int8_t cpu_id;          // 正在使用此页的 CPU（-1 = 无）
      struct Page *partial_next; // partial list 链接
      struct Page *partial_prev;
    } slab;
  };
} Page;

// BFC allocator (prefix functions + global variables)
extern Page *bfc_frames;
extern Page *bfc_free_list;

void bfc_init(void);
Page *bfc_alloc_page(size_t n);
Page *bfc_alloc_page_low(size_t n);
Page *bfc_free_page(Page *page, size_t n);
size_t bfc_free_page_nums(void);

// ===================== Global variables =====================
extern size_t total_page_frames;

typedef struct boot_info boot_info;

void init_mem(boot_info *bi);

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

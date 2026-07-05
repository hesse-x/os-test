#ifndef KERNEL_MEM_ALLOC_H
#define KERNEL_MEM_ALLOC_H

#include "kernel/xcore/sparse.h"
#include "kernel/xcore/spinlock.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "boot/boot.h"
#include "kernel/xcore/atomic.h"

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
  refcount_t p_refcount; // physical page reference count (0=free, 1=exclusive,
                         // >1=shared)
  union {
    struct {
      size_t cont_page_num; // 连续页数（BFC 空闲/已分配大块）
      struct Page *prev;    // BFC free list 双向链表
      struct Page *next;
    } bfc;

    struct {
      struct kmem_cache_t *cache; // 所属 cache
      void *freelist;             // 空闲对象链表头（侵入式）
      uint32_t inuse;             // 已分配对象数
      uint32_t obj_count;         // 页内总对象数
      int8_t cpu_id;              // 正在使用此页的 CPU（-1 = 无）
      struct Page *partial_next;  // partial list 链接
      struct Page *partial_prev;
    } slab;
  };
} Page;

// BFC allocator (prefix functions + global variables)
extern Page *bfc_frames;
extern Page *bfc_free_list;

void bfc_init(void) __attribute__((no_sanitize("kernel-address")));
Page *bfc_alloc_page(size_t n) __attribute__((no_sanitize("kernel-address")));
Page *bfc_alloc_page_low(size_t n)
    __attribute__((no_sanitize("kernel-address")));
Page *bfc_free_page(Page *page, size_t n)
    __attribute__((no_sanitize("kernel-address")));
size_t bfc_free_page_nums(void) __attribute__((no_sanitize("kernel-address")));

// Convenience wrappers: return/accept data-page virtual address directly.
// Use these when the caller wants the writable page data, not the Page
// metadata. Avoids the common bug of feeding a Page* to code that expects a
// data pointer (e.g. fxsave, memset on a kernel stack buffer).
void *bfc_alloc_page_data(size_t n)
    __attribute__((no_sanitize("kernel-address")));
void bfc_free_page_data(void *data, size_t n)
    __attribute__((no_sanitize("kernel-address")));

// ===================== Global variables =====================
extern size_t total_page_frames;

typedef struct boot_info boot_info;

void init_mem(boot_info *bi) __attribute__((no_sanitize("kernel-address")));

extern spinlock_t bfc_lock;

// ===================== Address conversion =====================
phys_addr_t page_to_phys(Page *p)
    __attribute__((no_sanitize("kernel-address")));
kern_vaddr_t phys_to_virt(phys_addr_t phys)
    __attribute__((no_sanitize("kernel-address")));

// ===================== User page mapping =====================
uint64_t *ensure_pd(uint64_t *new_pml4, uint64_t vaddr);
uint64_t *ensure_pt_in_pd(uint64_t *pd_or_pdpt, uint64_t vaddr, int level);
bool map_user_page_direct(uint64_t *new_pml4, uint64_t vaddr, uint64_t phys,
                          uint64_t flags) __must_check;
bool map_user_pages(uint64_t *pml4, uint64_t vaddr_start, uint64_t vaddr_end,
                    uint64_t flags, int *pages_mapped) __must_check;
void unmap_user_pages(uint64_t *pml4, uint64_t vaddr_start, uint64_t vaddr_end,
                      int count);
uint64_t *lookup_pte(uint64_t cr3_phys, uint64_t vaddr);

#endif // KERNEL_MEM_ALLOC_H

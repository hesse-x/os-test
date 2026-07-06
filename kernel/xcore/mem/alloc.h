/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

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
  PAGE_FREE,    // free
  PAGE_USED,    // in use (e.g. kernel heap)
  PAGE_SLAB,    // slab page
  PAGE_RESERVED // reserved (hardware/BIOS)
} page_status;

struct page {
  page_status status;
  refcount_t p_refcount; // physical page reference count (0=free, 1=exclusive,
                         // >1=shared)
  union {
    struct {
      size_t cont_page_num; // contiguous page count (BFC free/allocated large
                            // block)
      struct page *prev;    // BFC free list (doubly-linked)
      struct page *next;
    } bfc;

    struct {
      struct kmem_cache *cache;  // owning cache
      void *freelist;            // free object list head (intrusive)
      uint32_t inuse;            // allocated object count
      uint32_t obj_count;        // total objects in page
      int8_t cpu_id;             // CPU currently using this page (-1 = none)
      struct page *partial_next; // partial list link
      struct page *partial_prev;
    } slab;
  };
};

// BFC allocator (prefix functions + global variables)
extern struct page *bfc_frames;
extern struct page *bfc_free_list;

void bfc_init(void) __attribute__((no_sanitize("kernel-address")));
struct page *bfc_alloc_page(size_t n)
    __attribute__((no_sanitize("kernel-address")));
struct page *bfc_alloc_page_low(size_t n)
    __attribute__((no_sanitize("kernel-address")));
struct page *bfc_free_page(struct page *page, size_t n)
    __attribute__((no_sanitize("kernel-address")));
size_t bfc_free_page_nums(void) __attribute__((no_sanitize("kernel-address")));

// Convenience wrappers: return/accept data-page virtual address directly.
// Use these when the caller wants the writable page data, not the page
// metadata. Avoids the common bug of feeding a struct page * to code that
// expects a data pointer (e.g. fxsave, memset on a kernel stack buffer).
void *bfc_alloc_page_data(size_t n)
    __attribute__((no_sanitize("kernel-address")));
void bfc_free_page_data(void *data, size_t n)
    __attribute__((no_sanitize("kernel-address")));

// ===================== Global variables =====================
extern size_t total_page_frames;

typedef struct boot_info boot_info;

void init_mem(boot_info *bi) __attribute__((no_sanitize("kernel-address")));

extern spinlock bfc_lock;

// ===================== Address conversion =====================
phys_addr_t page_to_phys(struct page *p)
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

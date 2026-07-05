/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/xcore/mem/slab.h"
#include "arch/x64/memlayout.h"
#include "arch/x64/paging.h"
#include "arch/x64/smp.h"
#include "arch/x64/utils.h"
#include "utils/macro.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/mem/kasan.h"
#include "xos/syscall.h"
#include <stdint.h>

// Global kmalloc cache array
kmem_cache kmalloc_caches[NUM_KMALLOC_CLASSES];

// Global kernel memory statistics
struct kernel_mem_stats kernel_mem_stats;

// Object size for each size class
static const size_t class_sizes[NUM_KMALLOC_CLASSES] = {
    8, 16, 32, 64, 128, 256, 512, 1024, 2048};

// ===================== Slab page initialization =====================
__attribute__((no_sanitize("kernel-address"))) static void
slab_page_init(Page *page, kmem_cache *cache, int cpu_id) {
  page->status = PAGE_SLAB;
  page->slab.cache = cache;
  page->slab.inuse = 0;
  page->slab.obj_count = PAGE_SIZE / cache->obj_size;
  page->slab.cpu_id = (int8_t)cpu_id;
  page->slab.freelist = NULL;
  page->slab.partial_next = NULL;
  page->slab.partial_prev = NULL;

  // Intrusive freelist: the first 8 bytes of each free object store the next
  // pointer
  char *base =
      (__force char *)phys_to_virt((__force phys_addr_t)page_to_phys(page));
  for (uint32_t i = 0; i < page->slab.obj_count; i++) {
    void *obj = base + i * cache->obj_size;
    *(void **)obj = page->slab.freelist;
    page->slab.freelist = obj;
  }
}

// ===================== partial list operations =====================
__attribute__((no_sanitize("kernel-address"))) static void
partial_add(kmem_cache *cache, Page *page) {
  page->slab.partial_next = cache->partial;
  page->slab.partial_prev = NULL;
  if (cache->partial) {
    cache->partial->slab.partial_prev = page;
  }
  cache->partial = page;
}

__attribute__((no_sanitize("kernel-address"))) static void
partial_remove(kmem_cache *cache, Page *page) {
  if (page->slab.partial_prev) {
    page->slab.partial_prev->slab.partial_next = page->slab.partial_next;
  } else {
    cache->partial = page->slab.partial_next;
  }
  if (page->slab.partial_next) {
    page->slab.partial_next->slab.partial_prev = page->slab.partial_prev;
  }
  page->slab.partial_next = NULL;
  page->slab.partial_prev = NULL;
}

// ===================== slab_init =====================
__attribute__((no_sanitize("kernel-address"))) void slab_init() {
  for (int i = 0; i < NUM_KMALLOC_CLASSES; i++) {
    kmalloc_caches[i].obj_size = class_sizes[i];
    kmalloc_caches[i].redzone_size = 0;
    kmalloc_caches[i].lock = SPINLOCK_INIT;
    kmalloc_caches[i].partial = NULL;
  }
  // Initialize kernel_mem_stats
  extern size_t total_page_frames; // from kernel/mem/alloc.c
  memstat_set(&kernel_mem_stats.total_pages, (int)total_page_frames);
  memstat_set(&kernel_mem_stats.used_pages, 0);
  memstat_set(&kernel_mem_stats.kmalloc_calls, 0);
  memstat_set(&kernel_mem_stats.kfree_calls, 0);
  memstat_set(&kernel_mem_stats.slab_used_bytes, 0);
  kernel_mem_stats.slab_peak_bytes = 0;
  printk(LOG_INFO, "slab_init: ok\n");
}

// ===================== slab poison (debug build) =====================
// In debug builds, fill freshly-allocated and freshly-freed slab objects with
// 0xAA. This turns "out-of-bounds read returns a plausible-looking pointer"
// into "OOB read returns 0xAAAAAAAAAAAAAAAA", which is obviously garbage and
// faults immediately when dereferenced. Zero cost in release builds.
//
// This is deliberately NOT a redzone between objects — it fills the whole
// object, so it also catches use-after-free (freed object becomes 0xAA) and
// uninitialized-field bugs (caller sees 0xAA... instead of a stale pointer
// left over from the freelist linked list or a previous allocation).
#ifndef NDEBUG
#define SLAB_POISON_VALUE 0xAA
static inline void slab_poison_alloc(void *obj, size_t obj_size) {
  __memset(obj, SLAB_POISON_VALUE, obj_size);
}
static inline void slab_poison_free(void *obj, size_t obj_size) {
  __memset(obj, SLAB_POISON_VALUE, obj_size);
}
#else
static inline void slab_poison_alloc(void *obj, size_t obj_size) {
  (void)obj;
  (void)obj_size;
}
static inline void slab_poison_free(void *obj, size_t obj_size) {
  (void)obj;
  (void)obj_size;
}
#endif

// ===================== kmalloc =====================
__attribute__((no_sanitize("kernel-address"))) void *kmalloc(size_t size) {
  if (size == 0)
    return NULL;

  memstat_inc(&kernel_mem_stats.kmalloc_calls);

  // Large allocation: go through BFC
  if (size > 2048) {
    size_t npages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    Page *page = bfc_alloc_page(npages);
    if (!page) {
      printk(LOG_WARN,
             "kmalloc(%zu) failed, slab_used=%d, free_pages=%d, caller=%p\n",
             size, memstat_read(&kernel_mem_stats.slab_used_bytes),
             memstat_read(&kernel_mem_stats.total_pages) -
                 memstat_read(&kernel_mem_stats.used_pages),
             __builtin_return_address(0));
      return NULL;
    }
    memstat_add(&kernel_mem_stats.slab_used_bytes, (int)(npages * PAGE_SIZE));
    void *ptr =
        (__force void *)phys_to_virt((__force phys_addr_t)page_to_phys(page));
    return ptr;
  }

  int c = size_to_class(size);
  kmem_cache *cache = &kmalloc_caches[c];
  cpu_local *cpu = get_cpu_local();
  Page *active = cpu->active_slab[c];

  // All paths hold lock (option A: remove fast path lock-free optimization)
  uint64_t flags;
  spin_lock_irqsave(&cache->lock, &flags);

  // Check active slab (former fast path, now lock-held)
  if (active && active->slab.freelist) {
    void *obj = active->slab.freelist;
    active->slab.freelist = *(void **)obj;
    active->slab.inuse++;
    spin_unlock_irqrestore(&cache->lock, flags);
    kasan_slab_alloc(obj, cache->obj_size);
    slab_poison_alloc(obj, cache->obj_size);
    memstat_add(&kernel_mem_stats.slab_used_bytes, (int)cache->obj_size);
    return obj;
  }

  // Slow path: check partial list (already under lock, no extra locking needed)
  while (cache->partial) {
    Page *page = cache->partial;
    if (page->slab.freelist == NULL) {
      // Page is actually full — remove from partial and skip
      partial_remove(cache, page);
      continue;
    }
    partial_remove(cache, page);
    page->slab.cpu_id = (int8_t)cpu->cpu_id;
    cpu->active_slab[c] = page;

    void *obj = page->slab.freelist;
    page->slab.freelist = *(void **)obj;
    page->slab.inuse++;
    spin_unlock_irqrestore(&cache->lock, flags);
    kasan_slab_alloc(obj, cache->obj_size);
    slab_poison_alloc(obj, cache->obj_size);
    memstat_add(&kernel_mem_stats.slab_used_bytes, (int)cache->obj_size);
    return obj;
  }

  // partial is also empty: allocate a new page from BFC
  Page *new_page = bfc_alloc_page(1);
  if (!new_page) {
    spin_unlock_irqrestore(&cache->lock, flags);
    printk(LOG_WARN,
           "kmalloc(%zu) failed (slab path), slab_used=%d, free_pages=%d, "
           "caller=%p\n",
           size, memstat_read(&kernel_mem_stats.slab_used_bytes),
           memstat_read(&kernel_mem_stats.total_pages) -
               memstat_read(&kernel_mem_stats.used_pages),
           __builtin_return_address(0));
    return NULL;
  }

  slab_page_init(new_page, cache, cpu->cpu_id);
  cpu->active_slab[c] = new_page;

  void *obj = new_page->slab.freelist;
  new_page->slab.freelist = *(void **)obj;
  new_page->slab.inuse++;
  spin_unlock_irqrestore(&cache->lock, flags);
  kasan_slab_alloc(obj, cache->obj_size);
  slab_poison_alloc(obj, cache->obj_size);
  memstat_add(&kernel_mem_stats.slab_used_bytes, (int)cache->obj_size);
  return obj;
}

// ===================== kfree =====================
__attribute__((no_sanitize("kernel-address"))) void kfree(const void *ptr) {
  if (!ptr)
    return;

  memstat_inc(&kernel_mem_stats.kfree_calls);

  uint64_t addr = (uint64_t)ptr;
  uint64_t phys = (__force uint64_t)PHY_ADDR(addr);
  Page *page = &bfc_frames[PHY_TO_PAGE(phys)];

  if (page->status == PAGE_USED) {
    // BFC large allocation free
    kasan_bfc_free(ptr, page->bfc.cont_page_num * PAGE_SIZE);
    memstat_sub(&kernel_mem_stats.slab_used_bytes,
                (int)(page->bfc.cont_page_num * PAGE_SIZE));
    bfc_free_page(page, page->bfc.cont_page_num);
    return;
  }

  if (page->status != PAGE_SLAB) {
    // page->status is neither PAGE_USED nor PAGE_SLAB, meaning the Page
    // descriptor has been corrupted. Unrecoverable; DEBUG panics directly to
    // locate, release builds do not check.
#ifndef NDEBUG
    printk(LOG_ERROR,
           "kfree: bad page status ptr=%p phys=%lx page=%p status=%d "
           "sizeof(Page)=%zu bfc_frames=%p\n",
           ptr, phys, page, page->status, sizeof(Page), bfc_frames);
    printk(
        LOG_ERROR,
        "  page desc: refcount=%d cache=%p freelist=%p inuse=%u obj_count=%u\n",
        refcount_read(&page->p_refcount), page->slab.cache, page->slab.freelist,
        page->slab.inuse, page->slab.obj_count);
    uint8_t *raw = (uint8_t *)page;
    printk(LOG_ERROR,
           "  page raw: %02x %02x %02x %02x | %02x %02x %02x %02x | %02x %02x "
           "%02x %02x %02x %02x %02x %02x | %02x %02x %02x %02x %02x %02x %02x "
           "%02x\n",
           raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7],
           raw[8], raw[9], raw[10], raw[11], raw[12], raw[13], raw[14], raw[15],
           raw[16], raw[17], raw[18], raw[19], raw[20], raw[21], raw[22],
           raw[23]);
#endif
    ASSERT(page->status == PAGE_SLAB);
    return;
  }

  kasan_slab_free(ptr, page->slab.cache->obj_size);
  slab_poison_free((void *)ptr, page->slab.cache->obj_size);

  kmem_cache *cache = page->slab.cache;
  int my_cpu = get_cpu_local()->cpu_id;

  if (page->slab.cpu_id == my_cpu) {
    // Same CPU: hold the lock throughout (option A: eliminate data race)
    uint64_t flags;
    spin_lock_irqsave(&cache->lock, &flags);
    *(void **)ptr = page->slab.freelist;
    page->slab.freelist = (void *)ptr;
    page->slab.inuse--;
    memstat_sub(&kernel_mem_stats.slab_used_bytes, (int)cache->obj_size);

    if (page->slab.inuse == page->slab.obj_count - 1) {
      partial_add(cache, page);
    }
    spin_unlock_irqrestore(&cache->lock, flags);
  } else {
    // Cross-CPU free: hold the lock
    uint64_t flags;
    spin_lock_irqsave(&cache->lock, &flags);
    *(void **)ptr = page->slab.freelist;
    page->slab.freelist = (void *)ptr;
    page->slab.inuse--;
    memstat_sub(&kernel_mem_stats.slab_used_bytes, (int)cache->obj_size);

    if (page->slab.inuse == page->slab.obj_count - 1) {
      partial_add(cache, page);
    }
    spin_unlock_irqrestore(&cache->lock, flags);
  }
}

// ===================== kcalloc =====================
__attribute__((no_sanitize("kernel-address"))) void *kcalloc(size_t n,
                                                             size_t size) {
  if (n && size && n > (size_t)-1 / size)
    return NULL;
  size_t total = n * size;
  void *p = kmalloc(total);
  if (p) {
    char *dst = (char *)p;
    for (size_t i = 0; i < total; i++)
      dst[i] = 0;
  }
  return p;
}

// ===================== krealloc =====================
__attribute__((no_sanitize("kernel-address"))) void *krealloc(void *ptr,
                                                              size_t new_size) {
  if (!ptr)
    return kmalloc(new_size);
  if (new_size == 0) {
    kfree(ptr);
    return NULL;
  }

  // Get the old size
  uint64_t addr = (uint64_t)ptr;
  uint64_t phys = (__force uint64_t)PHY_ADDR(addr);
  Page *page = &bfc_frames[PHY_TO_PAGE(phys)];

  size_t old_size;
  if (page->status == PAGE_SLAB) {
    old_size = page->slab.cache->obj_size;
  } else {
    old_size = page->bfc.cont_page_num * PAGE_SIZE;
  }

  void *new_ptr = kmalloc(new_size);
  if (!new_ptr)
    return NULL;

  size_t copy = old_size < new_size ? old_size : new_size;
  char *src = (char *)ptr;
  char *dst = (char *)new_ptr;
  for (size_t i = 0; i < copy; i++)
    dst[i] = src[i];

  kfree(ptr);
  return new_ptr;
}

// ===================== Dedicated slab cache API =====================
// Shares slab_page_init / partial list / kfree Page descriptor conventions
// with kmalloc. Differences: exact obj_size (not aligned up to a size class),
// no per-CPU active_slab (only partial list + new page allocation). Streamlined
// version: no ctor/align/flags.
//
// obj_size upper bound constraint: must be <= PAGE_SIZE/2, otherwise a page
// cannot hold >= 2 objects (the intrusive freelist requires each object to
// fit a next pointer). xtask ~2KB satisfies this. kmalloc large objects go
// through BFC; dedicated caches should not handle such usage.

// kmem_cache_create: statically allocate the cache control block (kmem_cache
// itself is allocated via kmalloc). Returns NULL on kmalloc failure. The cache
// is held by the caller; no destroy needed (xtask_cache is global and
// persistent).
__attribute__((no_sanitize("kernel-address"))) kmem_cache *
kmem_cache_create(const char *name, size_t obj_size) {
  (void)name; // debug only, not stored for now
  if (obj_size == 0 || obj_size > PAGE_SIZE / 2)
    return NULL;
  kmem_cache *cache = (kmem_cache *)kmalloc(sizeof(kmem_cache));
  if (!cache)
    return NULL;
  cache->obj_size = obj_size;
  cache->redzone_size = 0;
  cache->lock = SPINLOCK_INIT;
  cache->partial = NULL;
  return cache;
}

// kmem_cache_alloc: allocate one object from the cache. Holds cache->lock
// throughout. Path: partial list has a free slot -> reuse; otherwise allocate
// a new page from BFC and initialize it.
__attribute__((no_sanitize("kernel-address"))) void *
kmem_cache_alloc(kmem_cache *cache) {
  uint64_t flags;
  spin_lock_irqsave(&cache->lock, &flags);

  // Find a page with free objects in the partial list
  while (cache->partial) {
    Page *page = cache->partial;
    if (page->slab.freelist == NULL) {
      // Page is full, remove from partial and skip
      partial_remove(cache, page);
      continue;
    }
    partial_remove(cache, page);
    void *obj = page->slab.freelist;
    page->slab.freelist = *(void **)obj;
    page->slab.inuse++;
    spin_unlock_irqrestore(&cache->lock, flags);
    kasan_slab_alloc(obj, cache->obj_size);
    slab_poison_alloc(obj, cache->obj_size);
    memstat_add(&kernel_mem_stats.slab_used_bytes, (int)cache->obj_size);
    return obj;
  }

  // partial is empty: allocate a new page
  Page *new_page = bfc_alloc_page(1);
  if (!new_page) {
    spin_unlock_irqrestore(&cache->lock, flags);
    printk(LOG_WARN, "kmem_cache_alloc(%zu) failed: no free pages\n",
           cache->obj_size);
    return NULL;
  }
  slab_page_init(new_page, cache, -1); // cpu_id=-1: dedicated cache has no per-CPU ownership

  void *obj = new_page->slab.freelist;
  new_page->slab.freelist = *(void **)obj;
  new_page->slab.inuse++;
  // Add the new page back to partial immediately (still has free objects),
  // reuse on next alloc
  partial_add(cache, new_page);

  spin_unlock_irqrestore(&cache->lock, flags);
  kasan_slab_alloc(obj, cache->obj_size);
  slab_poison_alloc(obj, cache->obj_size);
  memstat_add(&kernel_mem_stats.slab_used_bytes, (int)cache->obj_size);
  return obj;
}

// kmem_cache_free: free an object back to its page's freelist. Reverse-lookups
// the Page descriptor via the physical address. Same logic as the slab branch
// of kfree (intrusive list + inuse decrement + full->partial transition).
__attribute__((no_sanitize("kernel-address"))) void
kmem_cache_free(kmem_cache *cache, void *obj) {
  if (!obj)
    return;
  memstat_inc(&kernel_mem_stats.kfree_calls);

  uint64_t addr = (uint64_t)obj;
  uint64_t phys = (__force uint64_t)PHY_ADDR(addr);
  Page *page = &bfc_frames[PHY_TO_PAGE(phys)];

  // Object pages of a dedicated cache must be PAGE_SLAB; any other status
  // means obj does not belong to this cache (double-free/wild pointer)
  if (page->status != PAGE_SLAB) {
    printk(LOG_ERROR, "kmem_cache_free: bad page status ptr=%p status=%d\n",
           obj, page->status);
    ASSERT(page->status == PAGE_SLAB);
    return;
  }

  kasan_slab_free(obj, cache->obj_size);
  slab_poison_free(obj, cache->obj_size);

  uint64_t flags;
  spin_lock_irqsave(&cache->lock, &flags);
  *(void **)obj = page->slab.freelist;
  page->slab.freelist = obj;
  page->slab.inuse--;
  memstat_sub(&kernel_mem_stats.slab_used_bytes, (int)cache->obj_size);
  // If the page was full before the free (inuse==obj_count) it is not on
  // partial; add it back to partial after the count drops
  if (page->slab.inuse == page->slab.obj_count - 1) {
    partial_add(cache, page);
  }
  spin_unlock_irqrestore(&cache->lock, flags);
}

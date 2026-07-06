/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_MEM_SLAB_H
#define KERNEL_MEM_SLAB_H

#include "kernel/xcore/atomic.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/spinlock.h"
#include <stddef.h>
#include <stdint.h>
#include <xos/syscall.h> // struct kernel_mem_stats (shared kernel/user layout)

#define KMALLOC_SHIFT_LOW 3   // minimum class = 8B
#define KMALLOC_SHIFT_HIGH 11 // maximum class = 2048B

// ===================== Kernel memory accounting =====================
// struct kernel_mem_stats comes from xos/syscall.h (shared kernel/user
// layout). Kernel code accesses atomic fields via cast: (atomic_t*)&field,
// since atomic_t = { int counter } and layout is identical to plain int.
extern struct kernel_mem_stats kernel_mem_stats;

// Helpers for atomic access to kernel_mem_stats int fields (cast int* →
// atomic_t*)
static inline int memstat_read(int *field) {
  return atomic_read((atomic_t *)field);
}
static inline int memstat_add(int *field, int val) {
  return atomic_add_return((atomic_t *)field, val);
}
static inline int memstat_sub(int *field, int val) {
  return atomic_sub_return((atomic_t *)field, val);
}
static inline void memstat_inc(int *field) { atomic_inc((atomic_t *)field); }
static inline void memstat_set(int *field, int val) {
  atomic_set((atomic_t *)field, val);
}

typedef struct kmem_cache {
  size_t obj_size;     // object size
  size_t redzone_size; // redzone size (currently = 0)
  spinlock lock;     // per-cache lock (protects partial list)
  struct page *partial;       // slab list with free objects
} kmem_cache;

// Global kmalloc cache array
extern kmem_cache kmalloc_caches[NUM_KMALLOC_CLASSES];

static inline int size_to_class(size_t size) {
  if (size <= 8)
    return 0;
  if (size <= 16)
    return 1;
  if (size <= 32)
    return 2;
  if (size <= 64)
    return 3;
  if (size <= 128)
    return 4;
  if (size <= 256)
    return 5;
  if (size <= 512)
    return 6;
  if (size <= 1024)
    return 7;
  return 8; // <= 2048
}

void slab_init(void) __attribute__((no_sanitize("kernel-address")));
void *kmalloc(size_t size) __must_check
    __attribute__((no_sanitize("kernel-address")));
void kfree(const void *ptr) __attribute__((no_sanitize("kernel-address")));
void *kcalloc(size_t n, size_t size) __must_check
    __attribute__((no_sanitize("kernel-address")));
void *krealloc(void *ptr, size_t new_size) __must_check
    __attribute__((no_sanitize("kernel-address")));

// ===================== Dedicated slab cache API (Linux-style
// streamlined)===================== Dedicated cache for fixed-size,
// long-lived objects (e.g. xtask). Object pool reuse, zero internal
// fragmentation. Streamlined version: only name + obj_size, no
// ctor/align/flags reserved (keep it simple, extend later if needed).
//
// Difference from kmalloc: kmalloc uses fixed size classes (8..2048),
// object size aligned to class; kmem_cache_create uses exact obj_size to
// initialize in-page freelist, objects are more compact.
kmem_cache *kmem_cache_create(const char *name, size_t obj_size) __must_check
    __attribute__((no_sanitize("kernel-address")));
void *kmem_cache_alloc(kmem_cache *cache) __must_check
    __attribute__((no_sanitize("kernel-address")));
void kmem_cache_free(kmem_cache *cache, void *obj)
    __attribute__((no_sanitize("kernel-address")));

#endif // KERNEL_MEM_SLAB_H

#ifndef KERNEL_MEM_SLAB_H
#define KERNEL_MEM_SLAB_H

#include <stdint.h>
#include <stddef.h>
#include "kernel/sparse.h"
#include "kernel/spinlock.h"
#include "kernel/mem/alloc.h"

#define KMALLOC_SHIFT_LOW   3    // 最小 class = 8B
#define KMALLOC_SHIFT_HIGH  11   // 最大 class = 2048B

typedef struct kmem_cache_t {
    size_t obj_size;              // 对象大小
    size_t redzone_size;          // 红区大小（当前 = 0）
    spinlock_t lock;              // per-cache 锁（保护 partial list）
    Page *partial;                // 有空闲对象的 slab 链表
} kmem_cache_t;

// 全局 kmalloc cache 数组
extern kmem_cache_t kmalloc_caches[NUM_KMALLOC_CLASSES];

static inline int size_to_class(size_t size) {
    if (size <= 8)   return 0;
    if (size <= 16)  return 1;
    if (size <= 32)  return 2;
    if (size <= 64)  return 3;
    if (size <= 128) return 4;
    if (size <= 256) return 5;
    if (size <= 512) return 6;
    if (size <= 1024) return 7;
    return 8;  // <= 2048
}

void slab_init(void) __attribute__((no_sanitize("kernel-address")));
void *kmalloc(size_t size) __must_check __attribute__((no_sanitize("kernel-address")));
void kfree(const void *ptr) __attribute__((no_sanitize("kernel-address")));
void *kcalloc(size_t n, size_t size) __must_check __attribute__((no_sanitize("kernel-address")));
void *krealloc(void *ptr, size_t new_size) __must_check __attribute__((no_sanitize("kernel-address")));

#endif // KERNEL_MEM_SLAB_H

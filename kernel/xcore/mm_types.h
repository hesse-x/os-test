#ifndef KERNEL_XCORE_MM_TYPES_H
#define KERNEL_XCORE_MM_TYPES_H

// Memory management types shared between Xcore and BSD layers.
// These types are defined here so Xcore does not need to include kernel/bsd/ headers.

#include <stdint.h>
#include <stddef.h>
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/atomic.h"
#include "common/types.h"        // pid_t
#include "common/mman.h"         // PROT_*

// ===================== SHM =====================
#define SHM_SEALED  2

typedef struct shm {
    uint64_t phys;
    size_t   npages;
    size_t   file_size;
    refcount_t s_count;
    int      flags;
    uint32_t seals;
    char     name[32];
    uint64_t *page_list;
    int      num_pages;
} shm_t;

// ===================== Memory mapping region =====================
#define MAP_PHYSICAL_BASE 0x70000000

typedef struct mmap_region {
    uint64_t vaddr;
    uint64_t size;
    uint64_t phys;
    struct shm *shm_obj;
    uint32_t prot;
    struct mmap_region *next;
} mmap_region_t;

// ===================== Address space =====================
typedef struct mm_t {
    uint64_t cr3;
    refcount_t m_count;
    uint64_t mmap_brk;
    uint64_t mmap_phys_brk;
    struct mmap_region *mmap_regions;
    pid_t    parent_pid;
} mm_t;

void mm_release(mm_t *mm, pid_t owner_pid);

#endif // KERNEL_XCORE_MM_TYPES_H

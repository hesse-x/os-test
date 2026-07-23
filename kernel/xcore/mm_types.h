/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_XCORE_MM_TYPES_H
#define KERNEL_XCORE_MM_TYPES_H

// Memory management types shared between Xcore and BSD layers.
// These types are defined here so Xcore does not need to include kernel/bsd/
// headers.

#include "kernel/xcore/atomic.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/spinlock.h"
#include <stddef.h>
#include <stdint.h>
#include <xos/mman.h>  // PROT_*
#include <xos/types.h> // pid_t

// ===================== SHM =====================
#define SHM_SEALED 2

typedef struct shm {
  uint64_t phys;
  size_t npages;
  size_t file_size;
  refcount_t s_count;
  int flags;
  uint32_t seals;
  char name[32];
  uint64_t *page_list;
  int num_pages;
} shm;

// ===================== Memory mapping region =====================
#define MAP_PHYSICAL_BASE 0x70000000

// OS-internal mmap flag bits stored in mmap_region.flags (NOT in uapi mman.h,
// which only carries the standard Linux MAP_* set). Kept here next to the
// struct that records them so syscall.c and driver mmap paths share one
// definition instead of redefining the magic numbers.
#define KMAP_PHYSICAL                                                          \
  0x80000000u         /* MAP_PHYSICAL: map a fixed physical range              \
                       */
#define KMAP_UC 0x08u /* map uncacheable (device MMIO) */

typedef struct mmap_region {
  uint64_t vaddr;
  uint64_t size;
  uint64_t phys;       // MAP_PHYSICAL physical base, 0 = not PHYSICAL
  struct shm *shm_obj; // SHM mapping ref, non-NULL = SHM
  uint32_t prot;       // PROT_* bits
  int fd;              // -1 = anonymous; otherwise the mmap fd (ref NOT held)
  uint64_t offset;     // mmap offset (phys offset / file offset / 0)
  uint32_t flags;      // user MAP_* bits + OS-internal MAP_PHYSICAL/MAP_UC
  // S12: file-backed mmap. inode holds an inode_get reference for the lifetime
  // of the region so fault-in still works after close(fd) (Linux semantics);
  // NULL for anonymous/SHM/PHYSICAL. shm_private_src holds a shm_get reference
  // for memfd MAP_PRIVATE (COW from the shm page list); NULL otherwise. Both
  // are released in munmap/mm_release/execve and bumped in copy_mmap_regions.
  struct inode *inode;
  struct shm *shm_private_src;
  struct mmap_region *next;
} mmap_region;

// ===================== Address space =====================
typedef struct mm {
  uint64_t cr3;
  refcount_t m_count;
  uint64_t mmap_brk;
  uint64_t mmap_phys_brk;
  struct mmap_region *mmap_regions;
  pid_t parent_pid;
  spinlock mmap_lock;
} mm;

void mm_release(mm *mm, pid_t owner_pid);

#endif // KERNEL_XCORE_MM_TYPES_H

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef COMMON_SYSCALL_H
#define COMMON_SYSCALL_H

#include <stddef.h>

// ===================== Unified syscall return convention =====================
//   Success: return >= 0 (value meaning depends on syscall)
//   Failure: return -errno (negative value)
// All __syscallN return int64_t from kernel; wrappers cast as needed.
//
// This header is the UAPI shared between kernel and userspace: it carries only
// the return convention and shared structs. Syscall numbers live in
// xos/syscall_nums.h; the __syscallN inline-assembly wrappers live in
// xos/syscall_asm.h; the semantic user-space wrappers (sys_getpid, ...)
// live in user/include/syscall.h. Keeping these separate lets this header stay
// self-contained (no dependency on kernel-internal arch/x64/utils.h).

// ===================== Kernel memory stats (shared with user space)
// =====================
struct kernel_mem_stats {
  int total_pages;        // atomic_t: physical total pages
  int used_pages;         // atomic_t: pages allocated
  int slab_used_bytes;    // atomic_t: slab bytes in use
  size_t slab_peak_bytes; // slab peak usage
  int kmalloc_calls;      // atomic_t: kmalloc call count
  int kfree_calls;        // atomic_t: kfree call count
};

#endif // COMMON_SYSCALL_H

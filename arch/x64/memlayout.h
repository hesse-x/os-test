/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ARCH_X64_MEMLAYOUT_H
#define ARCH_X64_MEMLAYOUT_H

// x86-64 memory layout constants (shared kernel/user-space)
#define PAGE_SHIFT 12
#define PAGE_SIZE (1 << PAGE_SHIFT) // 4096
#define PAGE_SIZE_2M 0x200000

// Higher-half kernel/user boundary: user space lives below this, kernel above.
#define KERNEL_VMA_BOUNDARY 0xFFFFFFFF80000000ULL

// ld.so fixed base (below stack top 0x7FFFFFFFE000, fixed high address, no
// ASLR)
#define LD_SO_BASE 0x7FFFFF000000ULL

// User stack top (consistent with hardcoded values in proc.c / sched.c)
#define USER_STACK_TOP 0x00007FFFFFFFE000ULL

#define PHY_TO_PAGE(addr) ((addr) >> PAGE_SHIFT)
#define GET_PAGE_NUM(len) (((len) + PAGE_SIZE - 1) / PAGE_SIZE)

// Linker symbol: end of kernel image (used by allocators)
#include <stdint.h>
extern uint8_t kernel_end[];

#endif // ARCH_X64_MEMLAYOUT_H

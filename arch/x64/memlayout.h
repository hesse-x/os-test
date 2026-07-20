/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ARCH_X64_MEMLAYOUT_H
#define ARCH_X64_MEMLAYOUT_H

#include <xos/page.h> /* PAGE_SHIFT / PAGE_SIZE / PAGE_SIZE_2M (UAPI, shared kernel/user) */

// Higher-half kernel/user boundary: user space lives below this, kernel above.
#define KERNEL_VMA_BOUNDARY 0xFFFFFFFF80000000ULL

// ld.so fixed base (below stack top 0x7FFFFFFFE000, fixed high address, no
// ASLR)
#define LD_SO_BASE 0x7FFFFF000000ULL

// User stack top (consistent with hardcoded values in proc.c / sched.c)
#define USER_STACK_TOP 0x00007FFFFFFFE000ULL

#define PHY_TO_PAGE(addr) ((addr) >> PAGE_SHIFT)
#define GET_PAGE_NUM(len) (((len) + PAGE_SIZE - 1) / PAGE_SIZE)

// Kernel stack size. Each task gets KERNEL_STACK_PAGES * PAGE_SIZE of kernel
// stack. The previous 8KB (2 pages) overflowed under SMP IRQ nesting: a syscall
// enables interrupts (sti in syscall_fast_entry), so the LAPIC timer and AHCI
// completion IRQs (which itself re-sti) can nest multiple 176-byte trapframes
// plus deep C call chains (trap_dispatch -> sys_ioctl -> drm_ioctl ->
// virtgpu execbuf -> ...) on the same 8KB stack, walking past the bottom into
// neighboring heap objects (confirmed via a bottom-of-stack canary:
// bug4-STACK-OVF). Linux's x86_64 THREAD_SIZE is 16KB; match it. 16KB resolved
// the §4 crash: under KVM (true parallel SMP) all 17 test suites pass.
// (See bug.md §4, bug4-stack-overflow-root-cause.)
#define KERNEL_STACK_PAGES 4
#define KERNEL_STACK_SIZE (KERNEL_STACK_PAGES * PAGE_SIZE)

// Linker symbol: end of kernel image (used by allocators)
#include <stdint.h>
extern uint8_t kernel_end[];

#endif // ARCH_X64_MEMLAYOUT_H

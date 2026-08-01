/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ARCH_X64_MEMLAYOUT_H
#define ARCH_X64_MEMLAYOUT_H

#include <xos/page.h> // PAGE_SHIFT / PAGE_SIZE / PAGE_SIZE_2M (UAPI, shared kernel/user)

// Higher-half kernel/user boundary: user space lives below this, kernel above.
// Equals VMA_BASE (boot/boot.h): the direct-map window starts here, so every
// address >= this is kernel space. User pointers are validated against this.
#define KERNEL_VMA_BOUNDARY 0xFFFFFF8000000000ULL

// ld.so fixed base (below stack top 0x7FFFFFFFE000, fixed high address, no
// ASLR)
#define LD_SO_BASE 0x7FFFFF000000ULL

// User stack top (consistent with hardcoded values in proc.c / sched.c)
#define USER_STACK_TOP 0x00007FFFFFFFE000ULL

#define PHY_TO_PAGE(addr) ((addr) >> PAGE_SHIFT)
#define GET_PAGE_NUM(len) (((len) + PAGE_SIZE - 1) / PAGE_SIZE)

// Kernel stack size per task. 16KB matches Linux x86_64 THREAD_SIZE: the old
// 8KB overflowed under SMP IRQ nesting (syscall sti + LAPIC timer/AHCI IRQ
// re-sti nesting deep call chains on one 8KB stack — see bug4-STACK-OVF).
#define KERNEL_STACK_PAGES 4
#define KERNEL_STACK_SIZE (KERNEL_STACK_PAGES * PAGE_SIZE)

// (frame_opt.md block 4) Canary written to the bottom of every kernel task
// stack and per-CPU IRQ stack; verified at switch/IRQ-entry choke points to
// catch stack overruns into neighboring heap objects.
#define KSTACK_CANARY 0xC0FFEE42C0FFEE42ULL
#define IRQ_STACK_CANARY 0xDEADBEEFDEADBEEFULL
#define IRQ_STACK_BYTES (4 * PAGE_SIZE) // matches IRQ_STACK_PAGES in smp.c

// Linker symbol: end of kernel image (used by allocators)
#include <stdint.h>
// hidden visibility: kernel_end is a linker-script symbol with no in-TU
// definition. Under -fPIE clang routes such externs through a GOT load whose
// slot holds the higher-half VMA; --no-relax blocks the GOTPCREL->LEA
// relaxation. In enable_paging's physical-address phase (pre-load_cr3, UEFI
// identity map only) that yields the unmapped VMA instead of the physical
// address -> #PF on the first PT page write. hidden keeps it a local
// relocation so clang emits lea, computing RIP+disp32 = physical address
// while executing at physical RIP.
extern uint8_t kernel_end[] __attribute__((visibility("hidden")));

#endif // ARCH_X64_MEMLAYOUT_H

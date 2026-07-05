/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

# start.S - kernel entry point (x64 only)
# _start: physical address execution -> enable_paging -> gdt_init ->
#         ljmp -> _entry64 (virtual address continues)

.section .text
.code64
.global _start
.global _entry64
.type _start, @function

#define VMA_BASE 0xFFFFFFFF80000000  // mirrors boot/boot.h -- keep in sync

_start:
    # rdi = boot_info* (physical address, passed by EFI stub)

    # Set up physical-address stack
    lea stack_bottom(%rip), %rax
    addq $8192, %rax
    movq %rax, %rsp

    # r12 = boot_info* physical address, preserved across ljmp
    movq %rdi, %r12

    # Build page tables + load CR3
    call enable_paging

    # Configure GDT + lgdt
    call gdt_init

    # Set segment registers (GDT loaded; still at physical addresses,
    # identity map makes them accessible)
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    # Compute _entry64 virtual address: physical address + VMA_BASE
    lea _entry64(%rip), %rax
    addq $VMA_BASE, %rax

    # ljmp to virtual address
    pushq $0x08
    pushq %rax
    lretq

.code64
_entry64:
    # --- Virtual address execution ---

    # Switch to virtual-address stack
    lea stack_bottom(%rip), %rsp
    addq $8192, %rsp

    # boot_info physical address -> virtual address
    addq $VMA_BASE, %r12

    # Copy to g_boot_info
    movq %r12, %rsi
    lea g_boot_info(%rip), %rdi
    movq $128, %rcx
    rep movsb

    # rdi = &g_boot_info
    lea g_boot_info(%rip), %rdi

    call kernel_main

    cli
1:  hlt
    jmp 1b

.size _start, . - _start

    .section .note.GNU-stack,"",@progbits

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

#define VMA_BASE 0xFFFFFF8000000000  // mirrors boot/boot.h -- keep in sync

# Byte count copied from the UEFI boot_info buffer into g_boot_info below.
# Must equal BOOT_INFO_SIZE (and thus sizeof(boot_info)) in boot/boot.h,
# which a _Static_assert there pins to the real struct size. Copying more
# than sizeof(boot_info) overruns g_boot_info in BSS and clobbers the
# adjacent globals (device_vma_base / early_bump_end / gdt) with stale
# bytes from the source buffer — keep these in sync when boot_info grows.
.set BOOT_INFO_SIZE, 72

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

    # Compute _entry64 virtual address: physical address + VMA_BASE.
    # VMA_BASE is a full 64-bit non-sign-extendable immediate (canonical
    # high-half, not representable as a sign-extended 32-bit addq operand), so
    # load it with movabsq into %rcx then add.
    lea _entry64(%rip), %rax
    movabsq $VMA_BASE, %rcx
    addq %rcx, %rax

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

    # boot_info physical address -> virtual address (VMA_BASE is a 64-bit
    # non-sign-extend immediate; use movabsq, see the entry-jump note above).
    movabsq $VMA_BASE, %rcx
    addq %rcx, %r12

    # Copy to g_boot_info. Copy EXACTLY sizeof(boot_info) bytes, not a
    # rounded-up 128: g_boot_info sits in BSS immediately followed by
    # device_vma_base / early_bump_end / gdt, and enable_paging already wrote
    # early_bump_end during the physical phase. A 128-byte rep movsb overruns
    # boot_info and clobbers those neighbours with stale bytes from the UEFI
    # source buffer, zeroing early_bump_end and corrupting device_vma_base/gdt.
    # BOOT_INFO_SIZE is .set above to mirror boot/boot.h's #define of the same
    # name, which a _Static_assert there pins to sizeof(boot_info).
    movq %r12, %rsi
    lea g_boot_info(%rip), %rdi
    movq $BOOT_INFO_SIZE, %rcx
    rep movsb

    # rdi = &g_boot_info
    lea g_boot_info(%rip), %rdi

    call kernel_main

    cli
1:  hlt
    jmp 1b

.size _start, . - _start

    .section .note.GNU-stack,"",@progbits

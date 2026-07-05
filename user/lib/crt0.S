/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

# crt0.o — _start entry (shared by static and dynamic main ELF)
# ld.md §3.7.2 / plan_ld2b3 T4
# _start is not in libc.a/libc.so; it is a standalone product linked into every main
# ELF

.section .text
.global _start
_start:
    # Stack layout: [rsp]=argc, [rsp+8]=argv[0], ..., argv[argc]=NULL, envp, ..., auxv
    # Parameter mapping (custom ABI, reuses SysV's first 6 registers + stack for the
    # 7th):
    #   rdi = main, rsi = argc, rdx = argv,
    #   rcx = init_array_start, r8 = init_array_end,
    #   r9  = fini_array_start, [rsp+8] = fini_array_end
    # The init_array/fini_array ranges are passed by crt0 taking the addresses of the
    # main ELF's linker symbols, avoiding libc.so referencing the hidden symbols
    # __init_array_start/end across objects
    leaq main(%rip), %rdi        # main function address
    movq (%rsp), %rsi            # argc
    leaq 8(%rsp), %rdx           # argv
    leaq __init_array_start(%rip), %rcx
    leaq __init_array_end(%rip), %r8
    leaq __fini_array_start(%rip), %r9
    # 16-byte stack alignment (x86-64 SysV ABI: rsp%16==0 at call, rsp%16==8 at
    # callee entry)
    # The 7th parameter fini_array_end is passed on the stack: sub $16 keeps
    # alignment, mov to (%rsp)
    subq $16, %rsp
    leaq __fini_array_end(%rip), %rax
    movq %rax, (%rsp)
    call __libc_start_main
1:  hlt
    jmp 1b

    .section .note.GNU-stack,"",@progbits
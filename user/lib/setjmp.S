/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

# setjmp.S — pure-asm setjmp/longjmp (D12)
# plan_posix §5.7: save 8 callee-saved registers to jmp_buf:
#   rbx rbp r12 r13 r14 r15 rsp rip
# Does not save the signal mask (C-standard setjmp does not require it;
# sigsetjmp/siglongjmp see todo.md).
#
# jmp_buf layout (long long[8], 8-byte aligned):
#   [0] rbx  [1] rbp  [2] r12  [3] r13
#   [4] r14  [5] r15  [6] rsp  [7] rip
# rip is fetched from (%rsp) — the return address at the setjmp call site (pushed by
# the call instruction). longjmp restores rip from jmp_buf[7], equivalent to "returning
# to just after the setjmp call site".
#
# SysV ABI: rdi = env (jmp_buf), rsi = val (longjmp's return value).
# setjmp returns 0; longjmp returns val (val==0 is forced to return 1, as POSIX requires).

    .section .text

    .global setjmp
    .type setjmp, @function
setjmp:
    movq %rbx,  0(%rdi)
    movq %rbp,  8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)
    movq %rsp, 48(%rdi)
    movq (%rsp), %rax        # return address
    movq %rax, 56(%rdi)
    xorl %eax, %eax          # setjmp first return is 0
    ret

    .size setjmp, .-setjmp

    .global longjmp
    .type longjmp, @function
longjmp:
    movq  0(%rdi), %rbx
    movq  8(%rdi), %rbp
    movq 16(%rdi), %r12
    movq 24(%rdi), %r13
    movq 32(%rdi), %r14
    movq 40(%rdi), %r15
    movq 48(%rdi), %rsp
    movq 56(%rdi), %rax      # restore return address
    movq %rax, (%rsp)        # put it back on top of stack, ret pops it
    movl %esi, %eax          # return value = val
    testl %eax, %eax         # val==0 forced to return 1
    jnz 1f
    movl $1, %eax
1:
    ret

    .size longjmp, .-longjmp

    /* POSIX aliases (no underscore): behavior identical to setjmp/longjmp */
    .global _setjmp
    .type _setjmp, @function
_setjmp:
    movq %rbx,  0(%rdi)
    movq %rbp,  8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)
    movq %rsp, 48(%rdi)
    movq (%rsp), %rax
    movq %rax, 56(%rdi)
    xorl %eax, %eax
    ret
    .size _setjmp, .-_setjmp

    .global _longjmp
    .type _longjmp, @function
_longjmp:
    movq  0(%rdi), %rbx
    movq  8(%rdi), %rbp
    movq 16(%rdi), %r12
    movq 24(%rdi), %r13
    movq 32(%rdi), %r14
    movq 40(%rdi), %r15
    movq 48(%rdi), %rsp
    movq 56(%rdi), %rax
    movq %rax, (%rsp)
    movl %esi, %eax
    testl %eax, %eax
    jnz 2f
    movl $1, %eax
2:
    ret
    .size _longjmp, .-_longjmp

    .section .note.GNU-stack,"",@progbits

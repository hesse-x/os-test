/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

# setjmp.S — 纯汇编 setjmp/longjmp（D12）
# plan_posix §5.7：保存 8 个 callee-saved 寄存器到 jmp_buf：
#   rbx rbp r12 r13 r14 r15 rsp rip
# 不保存信号掩码（C 标准 setjmp 不要求；sigsetjmp/siglongjmp 见 todo.md）。
#
# jmp_buf 布局（long long[8]，8 字节对齐）：
#   [0] rbx  [1] rbp  [2] r12  [3] r13
#   [4] r14  [5] r15  [6] rsp  [7] rip
# rip 用 (%rsp) 取——setjmp 调用点的返回地址（call 指令压栈）。
# longjmp 把 rip 恢复到 jmp_buf[7]，等价于"返回到 setjmp 调用点之后"。
#
# SysV ABI：rdi = env（jmp_buf），rsi = val（longjmp 的返回值）。
# setjmp 返回 0；longjmp 返回 val（val==0 时强制返回 1，POSIX 要求）。

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
    movq (%rsp), %rax        # 返回地址
    movq %rax, 56(%rdi)
    xorl %eax, %eax          # setjmp 首次返回 0
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
    movq 56(%rdi), %rax      # 恢复返回地址
    movq %rax, (%rsp)        # 放回栈顶，ret 弹出它
    movl %esi, %eax          # 返回值 = val
    testl %eax, %eax         # val==0 强制返回 1
    jnz 1f
    movl $1, %eax
1:
    ret

    .size longjmp, .-longjmp

    /* POSIX 别名（无下划线）：行为与 setjmp/longjmp 完全一致 */
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

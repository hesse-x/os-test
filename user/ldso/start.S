/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

# ld.so 汇编入口（避免任何 C 运行时依赖）
# ld.md §3.2.3

.section .text
.hidden __dls3
.global _start
_start:
    # rsp 指向 argc，不动栈
    # 跳 C 函数 __dls3，传 rsp 作为参数
    # __dls3 标记 hidden，避免走 PLT（bootstrap 前 GOT 未填）
    movq %rsp, %rdi
    call __dls3
    # __dls3 不返回，跳主 ELF entry
    # 若返回则挂掉
1:  hlt
    jmp 1b

    .section .note.GNU-stack,"",@progbits
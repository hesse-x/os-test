/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

# ld.so assembly entry (avoids any C runtime dependency)
# ld.md §3.2.3

.section .text
.hidden __dls3
.global _start
_start:
    # rsp points to argc, do not touch the stack
    # jump to C function __dls3, passing rsp as argument
    # __dls3 marked hidden to avoid going through PLT (GOT not filled before bootstrap)
    movq %rsp, %rdi
    call __dls3
    # __dls3 does not return, jumps to main ELF entry
    # should it ever return, halt here
1:  hlt
    jmp 1b

    .section .note.GNU-stack,"",@progbits
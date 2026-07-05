/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

# trapentry.S - 64-bit common trap entry/exit, syscall, context switch
# vectors.S sends all traps here.
.text
.code64

# ===================== __alltraps / __trapret =====================
.globl __alltraps
__alltraps:
    # CPU 已自动 push: SS, RSP, RFLAGS, CS, RIP, [error code]
    # vector stub 已 push: trapno, [dummy error code if needed]

    # 保存所有通用寄存器
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    # 设置内核数据段
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es

    # swapgs if from user mode (CS=0x2B at offset 144)
    cmpw $0x2B, 144(%rsp)
    jne 1f
    swapgs
1:

    # 调用 trap_dispatch(rdi = &trapframe)
    movq %rsp, %rdi
    call trap_dispatch

    # __trapret: 恢复
.globl __trapret
__trapret:
    # 检查 pending signals 后再返回用户态
    # (此时 gs 仍指向 per-CPU 数据，swapgs 尚未执行)
    movq %rsp, %rdi
    call check_pending_signals

    # Load current_task->fs_base into MSR_FS_BASE (TLS base for user mode).
    # cpu_local._cur_proc is at GS:16; xtask_fs_base_offset is a linker symbol
    # holding offsetof(xtask, fs_base). wrmsr: ecx=MSR, edx=hi32, eax=lo32.
    movq %gs:16, %rax
    movq xtask_fs_base_offset(%rip), %rcx
    movq (%rax,%rcx), %rdx          # rdx = current_task->fs_base
    movl $0xC0000100, %ecx          # MSR_FS_BASE
    movl %edx, %eax                 # eax = lo32
    shrq $32, %rdx                  # rdx = hi32
    wrmsr

    # Disable interrupts before swapgs — if a timer interrupt fires between
    # swapgs and iretq, the ISR's conditional swapgs (checks CS=kernel) would
    # skip swapgs, leaving GS_BASE=0 and corrupting per-CPU state.
    # iretq will re-enable interrupts when it loads RFLAGS with IF=1.
    cli

    # swapgs if returning to user mode (CS=0x2B at offset 144)
    cmpw $0x2B, 144(%rsp)
    jne 1f
    swapgs
1:

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    # 跳过 trapno + err_code (各8字节)
    addq $16, %rsp

    iretq

# ===================== syscall fast entry/exit (SYSCALL/SYSRET) =====================
# SYSCALL instruction: RCX = return RIP, R11 = RFLAGS, CS/SS loaded from STAR.
# No stack switch, no swapgs — we must do it manually.
# We build the same trapframe_t as int 0x80 so xcall_dispatch can be shared.
#
# trapframe_t layout (C struct, offsets from base):
#   0:   r15, r14, r13, r12, r11, r10, r9, r8
#   64:  rdi, rsi, rbp, rdx, rcx, rbx, rax
#   120: trapno, err_code
#   136: rip, cs, rflags, rsp, ss
#   Total = 176 bytes

.globl syscall_fast_entry
syscall_fast_entry:
    # SYSCALL entered kernel mode. We're still on user RSP.
    # RCX = user return RIP, R11 = user RFLAGS.
    # R10 = 4th syscall argument (arg4) — must preserve before using R10 as scratch.

    # Save user R10 (arg4) to per-CPU scratch slot (cpu_local.saved_r10 at GS:0)
    swapgs
    movq %r10, %gs:0

    # Now R10 is free — use it to save user RSP before switching stacks
    movq %rsp, %r10
    movq %gs:48, %rsp          # switch to per-process kernel stack (tss_rsp0 at offset 48)

    # Build trapframe on kernel stack (176 bytes)
    subq $176, %rsp

    # Store CPU-saved fields
    movq $0x23, 168(%rsp)      # ss = USER_DS
    movq %r10, 160(%rsp)       # rsp = user RSP (was in R10)
    movq %r11, 152(%rsp)       # rflags (was in R11)
    movq $0x2B, 144(%rsp)      # cs = USER_CS
    movq %rcx, 136(%rsp)       # rip (was in RCX)
    movq $0, 128(%rsp)         # err_code = 0
    movq $128, 120(%rsp)       # trapno = 128

    # Save all GP registers in trapframe order
    movq %r15, 0(%rsp)
    movq %r14, 8(%rsp)
    movq %r13, 16(%rsp)
    movq %r12, 24(%rsp)
    movq %r11, 32(%rsp)
    movq %gs:0, %r10           # restore original user R10 (arg4) from scratch
    movq %r10, 40(%rsp)        # save to trapframe r10 slot
    movq %r9, 48(%rsp)
    movq %r8, 56(%rsp)
    movq %rdi, 64(%rsp)
    movq %rsi, 72(%rsp)
    movq %rbp, 80(%rsp)
    movq %rdx, 88(%rsp)
    movq %rcx, 96(%rsp)
    movq %rbx, 104(%rsp)
    movq %rax, 112(%rsp)

    # Load kernel data segments
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es

    # Call xcall_dispatch(rdi = &trapframe)
    movq %rsp, %rdi
    call xcall_dispatch

    # Check pending signals before returning to user mode
    movq %rsp, %rdi
    call check_pending_signals

    # Load current_task->fs_base into MSR_FS_BASE (TLS base for user mode).
    # Same logic as __trapret; see comment there.
    movq %gs:16, %rax
    movq xtask_fs_base_offset(%rip), %rcx
    movq (%rax,%rcx), %rdx          # rdx = current_task->fs_base
    movl $0xC0000100, %ecx          # MSR_FS_BASE
    movl %edx, %eax                 # eax = lo32
    shrq $32, %rdx                  # rdx = hi32
    wrmsr

    # After xcall_dispatch returns, RSP = trapframe base (C ABI restores RSP).
    # Use SYSRET for fast return to user mode.
    # Note: blocking syscalls (getc) also return here after being rescheduled,
    # because schedule() -> switch_to saves/restores the full call chain.

    ## Disable interrupts before swapgs — same swapgs race as __trapret:
    ## if an interrupt fires between swapgs and sysretq with GS_BASE=0,
    ## the ISR runs with broken per-CPU pointers.
    cli

    # Restore GP registers from trapframe
    movq 112(%rsp), %rax
    movq 104(%rsp), %rbx
    # rcx and r11 restored below from rip/rflags for SYSRET
    movq 88(%rsp), %rdx
    movq 80(%rsp), %rbp
    movq 72(%rsp), %rsi
    movq 64(%rsp), %rdi
    movq 56(%rsp), %r8
    movq 48(%rsp), %r9
    movq 40(%rsp), %r10
    movq 24(%rsp), %r12
    movq 16(%rsp), %r13
    movq 8(%rsp), %r14
    movq 0(%rsp), %r15

    # Load SYSRET return state
    movq 136(%rsp), %rcx      # user RIP (SYSRET loads RCX → RIP)
    movq 152(%rsp), %r11      # user RFLAGS (SYSRET loads R11 → RFLAGS)
    movq 160(%rsp), %rsp      # user RSP — must be last before sysretq

    # swapgs back: GS_BASE(cpu_local) ↔ KERNEL_GS_BASE(0)
    swapgs

    sysretq

# ===================== context switch =====================
# void switch_to(task_t *prev, task_t *next)
# System V AMD64: rdi = prev, rsi = next
# task_t offsets validated by _Static_assert in kernel/proc.c
.globl switch_to
switch_to:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, 8(%rdi)       /* prev->k_rsp = RSP  */
    movq 8(%rsi), %rsp       /* RSP = next->k_rsp  */
    movq 24(%rsi), %rax      /* rax = next->cr3     */
    movq %rax, %cr3

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    ret

# process_entry: first return target for new processes
.globl process_entry
process_entry:
    jmp __trapret

# reload_cs: 用 lretq 刷新 CS 到 0x08
.globl reload_cs
reload_cs:
    subq $16, %rsp
    leaq 1f(%rip), %rax
    movq %rax, (%rsp)
    movq $0x08, 8(%rsp)
    lretq
1:
    ret

    .section .note.GNU-stack,"",@progbits
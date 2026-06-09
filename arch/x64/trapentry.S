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
# We build the same trapframe_t as int 0x80 so syscall_dispatch can be shared.
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
    # No swapgs or stack switch happened yet.

    # Save user RSP before we lose it (R10 is scratch, SYSCALL doesn't preserve it)
    movq %rsp, %r10

    # swapgs: GS_BASE(0) ↔ KERNEL_GS_BASE(cpu_local*) → GS_BASE = cpu_local*
    swapgs

    # Switch to kernel stack from cpu_local.tss_rsp0 (offset 32)
    # tss_rsp0 is updated by schedule() to the current process's kernel stack
    movq %gs:32, %rsp

    # Build trapframe on kernel stack (176 bytes)
    subq $176, %rsp

    # Store CPU-saved fields
    movq $0x23, 168(%rsp)     # ss = USER_DS
    movq %r10, 160(%rsp)      # rsp = user RSP (saved in R10)
    movq %r11, 152(%rsp)      # rflags (was in R11)
    movq $0x2B, 144(%rsp)     # cs = USER_CS
    movq %rcx, 136(%rsp)      # rip (was in RCX)
    movq $0, 128(%rsp)        # err_code = 0
    movq $128, 120(%rsp)      # trapno = 128

    # Save all GP registers in trapframe order
    movq %r15, 0(%rsp)
    movq %r14, 8(%rsp)
    movq %r13, 16(%rsp)
    movq %r12, 24(%rsp)
    movq %r11, 32(%rsp)
    movq %r10, 40(%rsp)
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

    # Call syscall_dispatch(rdi = &trapframe)
    movq %rsp, %rdi
    call syscall_dispatch

    # After syscall_dispatch returns, RSP = trapframe base (C ABI restores RSP).
    # Use SYSRET for fast return to user mode.
    # Note: blocking syscalls (getc) also return here after being rescheduled,
    # because schedule() -> switch_to saves/restores the full call chain.

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
# void switch_to(proc_t *prev, proc_t *next)
# System V AMD64: rdi = prev, rsi = next
# proc_t layout (64-bit, no padding):
#   pid(0) state(4) k_rsp(8) k_stack_top(16) cr3(24) entry(32) wait_event(40)
.globl switch_to
switch_to:
    # 保存 callee-saved 寄存器
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    # prev->k_rsp = current RSP
    movq %rsp, 8(%rdi)

    # RSP = next->k_rsp
    movq 8(%rsi), %rsp

    # Switch CR3 to next process's PML4
    movq 24(%rsi), %rax
    movq %rax, %cr3

    # 恢复 callee-saved 寄存器
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

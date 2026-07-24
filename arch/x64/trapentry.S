/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

# trapentry.S - 64-bit trap entry/exit, syscall entry, context switch
# vectors.S sends all traps here.
.text
.code64

# ===================== __irq_entry (hardware IRQs, frame_opt.md 块一) =====
# Mirror Linux common_interrupt: hardware IRQs (vector32..127, 236) switch to a
# per-CPU IRQ stack for the C handler, while pt_regs STAYS on the task stack.
# pt_regs (iret frame + 15 GPs) is always on the task stack; only the handler
# call chain + locals run on the IRQ stack.  Returning switches RSP back to the
# task stack and jumps to __trapret, which pops pt_regs + iretq from there.
#
# Why pt_regs stays on the task stack (not the IRQ stack): fixup_exception and
# deliver_signal rewrite tf->rip/rsp and rely on iretq popping that same frame.
# A kernel #PF (copy_from_user) takes __alltraps (stays on task stack); moving
# IRQ pt_regs to the IRQ stack would split tf from the iretq pop site.  Keeping
# pt_regs on the task stack for BOTH entry types makes user/kernel-origin IRQ
# handling identical (no per-origin branch) and leaves cur_tf/fixup/signal/iretq
# untouched.
#
# No nesting counter: every IDT gate here is an interrupt gate (IF=0 on entry)
# and 块二 removed the AHCI sti, so hard-IRQs never nest — the single
# irq_stack_saved_rsp slot is safe.  NMI uses IST1 and never reaches here.
.globl __irq_entry
__irq_entry:
    # 1. Push 15 GP registers onto the TASK stack (same order/offsets as
    #    __alltraps — trapno@120(%rsp), err_code@128, CS@144 after this).
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

    # Set kernel data segments
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es

    # 2. swapgs if from user mode (CS=0x2B at offset 144).  Done on the task
    #    stack BEFORE switching: subsequent movq %gs:irq_stack_top needs GS_BASE
    #    = cpu_local, which swapgs establishes for user-origin IRQs.  Kernel-
    #    origin IRQs already have GS_BASE=cpu_local (set at smp_apply_cpu), so
    #    skipping swapgs leaves gs correct either way.
    cmpw $0x2B, 144(%rsp)
    jne 1f
    swapgs
1:

    # 3. Save the task-stack RSP (top of pt_regs) and switch to the per-CPU IRQ
    #    stack.  Unconditional: hard-IRQs are IF=0 and never nest (块二), so the
    #    single irq_stack_saved_rsp slot cannot be overwritten.  IF=0 here is the
    #    load-bearing invariant — if a future ISR re-enables interrupts, this
    #    single-slot assumption silently breaks; 块二's might_sleep guard is the
    #    intended backstop, not a nesting counter here.
    movq cpu_local_irq_stack_saved_rsp_offset(%rip), %rcx
    movq %rsp, %gs:(%rcx)          # save task-stack RSP (pt_regs top)
    movq cpu_local_irq_stack_top_offset(%rip), %rcx
    movq %gs:(%rcx), %rsp          # RSP = per-CPU IRQ stack top

    # 4. Mark hard-IRQ context (might_sleep guard in schedule() / #PF sti gate).
    #    irq_stack_top is page-aligned (16-aligned); the call below pushes 8B,
    #    so trap_dispatch enters with RSP ≡ 8 mod 16 — SysV-ABI compliant, no
    #    extra sub $8 needed.
    movq cpu_local_in_hardirq_offset(%rip), %rcx
    incl %gs:(%rcx)                # in_hardirq++

    # 5. Call trap_dispatch(rdi = &trapframe).  rdi is the SAVED task-stack RSP
    #    (pt_regs base), NOT the current IRQ-stack RSP — the handler runs on the
    #    IRQ stack but tf points at the task-stack pt_regs.
    movq cpu_local_irq_stack_saved_rsp_offset(%rip), %rcx
    movq %gs:(%rcx), %rdi
    call trap_dispatch

    # 6. Handler returned: decrement in_hardirq (exit slow path runs with it 0,
    #    matching Linux irq_exit ordering) and switch RSP back to the task stack.
    movq cpu_local_in_hardirq_offset(%rip), %rcx
    decl %gs:(%rcx)                # in_hardirq--
    movq cpu_local_irq_stack_saved_rsp_offset(%rip), %rcx
    movq %gs:(%rcx), %rsp          # back to task-stack pt_regs

    # 7. Fall into the shared return path.  RSP now points at the task-stack
    #    pt_regs (same layout as __alltraps left), so check_pending_signals /
    #    fs_base load / pop regs / iretq all read the correct frame.  Any
    #    schedule() inside check_pending_signals runs on the task stack with
    #    in_hardirq==0 — the hard constraint that IRQ-stack scheduling would
    #    store the IRQ-stack RSP into prev->k_rsp.
    jmp __trapret

# ===================== __alltraps / __trapret =====================
.globl __alltraps
__alltraps:
    # CPU already pushed: SS, RSP, RFLAGS, CS, RIP, [error code]
    # vector stub pushed: trapno, [dummy error code if needed]

    # Save all GP registers
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

    # Set kernel data segments
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es

    # swapgs if from user mode (CS=0x2B at offset 144)
    cmpw $0x2B, 144(%rsp)
    jne 1f
    swapgs
1:
    # (frame_opt.md 块二) #PF sti gate — mirror Linux idtentry, which ENABLES
    # interrupts before calling do_page_fault.  #PF enters via an interrupt
    # gate (0x8E) so IF=0 on entry; the BSD file-fault handler below may block
    # in schedule(), which saves/restores the *entry* IF — running it with IF=0
    # would leave the task permanently interrupts-disabled on resume (deadlock).
    # So for trapno==14 (#PF) AND in_hardirq==0 (process context, not an
    # IRQ-body #PF) we sti before calling trap_dispatch, making the C handler
    # and its schedule() downstream run with IF=1.  An IRQ-context #PF
    # (in_hardirq>0) is an unrecoverable kernel bug: sti would let a nested
    # IRQ overwrite the single IRQ-stack RSP slot, so we keep IF=0 and let it
    # panic.  Only #PF is gated; other exceptions stay IF=0 (their handlers do
    # not block).  Hardware IRQs do not reach __alltraps after 块一 splits them
    # to __irq_entry; until then IRQs come here too but trapno>=32, so this
    # trapno==14 gate never fires for them.
    cmpq $14, 120(%rsp)           # trapno == 14 (#PF)?
    jne 2f
    movq cpu_local_in_hardirq_offset(%rip), %rcx
    movl %gs:(%rcx), %eax         # eax = in_hardirq (int)
    testl %eax, %eax
    jnz 2f                        # IRQ-context #PF: stay IF=0, let it panic
    sti                           # process-context #PF: enable interrupts
2:

    # Call trap_dispatch(rdi = &trapframe)
    movq %rsp, %rdi
    call trap_dispatch

    # __trapret: restore and return
.globl __trapret
__trapret:
    # Check pending signals before returning to user mode
    # (gs still points to per-CPU data; swapgs not yet executed)
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

    # Skip trapno + err_code (8 bytes each)
    addq $16, %rsp

    iretq

# ===================== syscall fast entry/exit (SYSCALL/SYSRET) =====================
# SYSCALL instruction: RCX = return RIP, R11 = RFLAGS, CS/SS loaded from STAR.
# No stack switch, no swapgs — we must do it manually.
# We build the same trapframe as int 0x80 so xcall_dispatch can be shared.
#
# trapframe layout (C struct, offsets from base):
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

    # Enable interrupts — timer IRQs must fire during syscall execution to
    # advance RCU grace periods (rcu_quiescent).  Without this, IF=0 prevents
    # timer IRQs from firing; on SMP, synchronize_rcu() busy-waits forever
    # because the other CPU never reaches a quiescent state.
    # __alltraps is swapgs-safe: it checks CS on the stack and skips swapgs
    # when the interrupted context is kernel mode (CS=0x08), so interrupts
    # firing between this sti and the cli in the SYSRET return path do not
    # corrupt GS_BASE.
    sti

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

# reload_cs: use lretq to reload CS to 0x08
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
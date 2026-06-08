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

    # swapgs if from user mode (CS=0x1B at offset 144)
    cmpw $0x1B, 144(%rsp)
    jne 1f
    swapgs
1:

    # 调用 trap_dispatch(rdi = &trapframe)
    movq %rsp, %rdi
    call trap_dispatch

    # __trapret: 恢复
.globl __trapret
__trapret:
    # swapgs if returning to user mode (CS=0x1B at offset 144)
    cmpw $0x1B, 144(%rsp)
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

# ===================== syscall entry/exit =====================
# int 0x80 from ring 3: CPU pushes SS, RSP, RFLAGS, CS, RIP
# then vector128 stub pushes err_code(0) + trapno(128)
# then we push all regs (same as __alltraps layout)
.globl syscall_entry
syscall_entry:
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
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es

    # int 0x80 always from user mode: swapgs
    swapgs

    movq %rsp, %rdi
    call syscall_dispatch

.globl syscall_ret
syscall_ret:
    # swapgs before returning to user mode
    swapgs

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
    addq $16, %rsp        /* skip trapno + err_code */
    iretq

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

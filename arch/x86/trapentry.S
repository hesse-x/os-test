# trapentry.S - common trap entry/exit
# vectors.S sends all traps here.
.text
.globl __alltraps
__alltraps:
    # push registers to build a trap frame
    # therefore make the stack look like a struct trapframe
    pushl %ds
    pushl %es
    pushl %fs
    pushl %gs
    pushal

    # load kernel data segment
    movl $0x10, %eax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    # push %esp to pass a pointer to the trapframe as an argument to trap_dispatch()
    pushl %esp

    # call trap_dispatch(tf), where tf=%esp
    call trap_dispatch

    # pop the pushed stack pointer
    popl %esp

    # return falls through to trapret...
.globl __trapret
__trapret:
    # restore registers from stack
    popal

    # restore %ds, %es, %fs and %gs
    popl %gs
    popl %fs
    popl %es
    popl %ds

    # get rid of the trap number and error code
    addl $0x8, %esp
    iret

# ===================== syscall entry/exit =====================
# int 0x80 from ring 3: CPU pushes SS, ESP, EFLAGS, CS, EIP
# then vector128 stub pushes error_code(0) + trapno(128)
# then we push segment regs + pushal (same as __alltraps layout)
# esp/ss are at the end of trapframe (after eflags), handled by CPU
.globl syscall_entry
syscall_entry:
    pushl %ds
    pushl %es
    pushl %fs
    pushl %gs
    pushal
    movl $0x10, %eax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    pushl %esp
    call syscall_dispatch
    addl $4, %esp

.globl syscall_ret
syscall_ret:
    popal
    popl %gs
    popl %fs
    popl %es
    popl %ds
    addl $8, %esp        /* skip trapno + err_code */
    iret                 /* pops EIP, CS, EFLAGS; on privilege change also ESP, SS */

# ===================== context switch =====================
# void switch_to(proc_t *prev, proc_t *next)
# proc_t layout: pid(0) state(4) k_esp(8) k_stack_top(12) cr3(16) entry(20) wait_event(24)
# C calling convention: prev at [esp+4], next at [esp+8] before callee-saved push
.globl switch_to
switch_to:
    pushl %ebx
    pushl %esi
    pushl %edi
    pushl %ebp

    # prev = original esp+4, after 4 pushes = current esp+20
    movl 20(%esp), %eax
    movl %esp, 8(%eax)           # prev->k_esp = current ESP

    # next = original esp+8, after 4 pushes = current esp+24
    movl 24(%esp), %ecx
    movl 8(%ecx), %esp           # ESP = next->k_esp

    # Switch CR3 to next process's page directory
    movl 16(%ecx), %eax          # next->cr3
    movl %eax, %cr3              # load CR3 (flushes TLB)

    popl %ebp
    popl %edi
    popl %esi
    popl %ebx
    ret

# process_entry: first return target for new processes
.globl process_entry
process_entry:
    jmp __trapret
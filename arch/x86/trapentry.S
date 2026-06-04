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
    addl $8, %esp        /* skip trapno + err_code */
    popl %gs
    popl %fs
    popl %es
    popl %ds
    iret                 /* pops EIP, CS, EFLAGS; on privilege change also ESP, SS */
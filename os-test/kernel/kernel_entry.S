#include "os-test/kernel/mem/memlayout.h"
#define REALLOC(x) (x - KERNEL_BASE_ADDR)

.text
.globl kernel_entry
.extern pde_init
.extern kernel_init
kernel_entry:
    movl $pde_init, %eax
    andl $0xFFFFFF, %eax
    pushl $REALLOC(kernel_pde)
    call *%eax
    addl $4, %esp
    popl %ebx

    movl $REALLOC(kernel_pde), %eax
    movl %eax, %cr3
    movl %cr0, %eax
    orl $0x80000000, %eax
    movl %eax, %cr0

    jmp relocated

relocated:

    # set ebp, esp
    movl $0x0, %ebp
    # the kernel stack region is from bootstack -- bootstacktop,
    # the kernel stack size is KSTACKSIZE (8KB)defined in memlayout.h
    movl $bootstacktop, %esp
    # now kernel stack is ready , call the first C function
    call kernel_init

# should never get here
spin:
    jmp spin

.data
.align PAGE_SIZE
    .globl bootstack
bootstack:
    .space KERNEL_STACK_SIZE
    .globl bootstacktop
bootstacktop:
kernel_pde:
    .space PAGE_SIZE

#include "os-test/utils/os_utils.h"
#include "os-test/kernel/mem/memlayout.h"
#include "os-test/kernel/mem/mmu.h"
#define REALLOC(x) (x - KERNEL_ENTRY_ADDR)

.text
.globl kernel_entry
.extern kernel_init
.extern update_gdt
kernel_entry:
    pushl %ebp
    movl %esp, %ebp

    movl 8(%ebp), %eax

    movl $__gdtdesc, %ebx
    subl $KERNEL_ENTRY_ADDR, %ebx
    addl %eax, %ebx
    pushl %ebx

    # set arg: uint32_t n
    pushl $0x3
    pushl %ebx
    pushl %eax

    leal update_gdt, %ebx
    subl $KERNEL_ENTRY_ADDR, %ebx
    addl %eax, %ebx

    call *%ebx
    addl $12, %esp

    popl %ebx
    lgdtl (%ebx)
    movl $KERNEL_DS, %eax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss

    call relocated
#    ljmp $KERNEL_CS, $relocated

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

.align 4
__gdt:
    SEG_NULL
    SEG_ASM(STA_X | STA_R, -KERNEL_ENTRY_ADDR, 0xFFFFFFFF)      # code segment
    SEG_ASM(STA_W, -KERNEL_ENTRY_ADDR, 0xFFFFFFFF)              # data segment
__gdtdesc:
    .word 0x17                                          # sizeof(__gdt) - 1
    .long REALLOC(__gdt)


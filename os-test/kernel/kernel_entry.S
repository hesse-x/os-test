#include "os-test/kernel/mem/mmu.h"
#include "os-test/kernel/mem/memlayout.h"
#define REALLOC(x) (x - KERNEL_ENTRY_ADDR)

.text
.globl kernel_entry
kernel_entry:
    # reload temperate gdt (second time) to remap all physical memory
    # virtual_addr 0~4G=linear_addr&physical_addr -KERNEL_ENTRY_ADDR~4G-KERNEL_ENTRY_ADDR 
    lgdt REALLOC(__gdtdesc)
    movl $KERNEL_DS, %eax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss

    ljmp $KERNEL_CS, $relocated

relocated:

    # set ebp, esp
    movl $0x0, %ebp
    # the kernel stack region is from bootstack -- bootstacktop,
    # the kernel stack size is KERNEL_STACK_SIZE (8KB)defined in memlayout.h
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
    SEG_ASM(STA_X | STA_R, - KERNEL_ENTRY_ADDR, 0xFFFFFFFF)      # code segment
    SEG_ASM(STA_W, - KERNEL_ENTRY_ADDR, 0xFFFFFFFF)              # data segment
__gdtdesc:
    .word 0x17                                          # sizeof(__gdt) - 1
    .long REALLOC(__gdt)


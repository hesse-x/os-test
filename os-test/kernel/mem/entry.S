#include "os-test/utils/os_utils.h"
#include "os-test/kernel/mem/memlayout.h"

#define SEG_NULL                                                \
    .word 0, 0;                                                 \
    .byte 0, 0, 0, 0

#define SEG_ASM(type,base,lim)                                  \
    .word (((lim) >> 12) & 0xffff), ((base) & 0xffff);          \
    .byte (((base) >> 16) & 0xff), (0x90 | (type)),             \
        (0xC0 | (((lim) >> 28) & 0xf)), (((base) >> 24) & 0xff)

#define REALLOC(x) (x - KERNBASE)

.text
.globl kernel_entry
.extern kernel_init
.extern update_gdt
kernel_entry:
    # 1. 保存旧的基址指针
    movl %esp, %ebp

    # 2. 获取参数
    movl 8(%ebp), %eax

    movl $__gdtdesc, %ebx
    subl $KERNEL_ENTRY_ADDR, %ebx
    addl %eax, %ebx
    pushl %ebx

    movl $0x3, %ecx
    pushl %ecx
    pushl %ebx
    pushl %eax

    leal update_gdt, %ebx
    subl $KERNEL_ENTRY_ADDR, %ebx
    addl %eax, %ebx
    call *%ebx

    popl %ebx

    lgdt %ebx
    movl $KERNEL_DS, %eax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss

    ljmp $KERNEL_CS, $relocated

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


.align 4
__gdt:
    SEG_NULL
    SEG_ASM(STA_X | STA_R, - KERNBASE, 0xFFFFFFFF)      # code segment
    SEG_ASM(STA_W, - KERNBASE, 0xFFFFFFFF)              # data segment
__gdtdesc:
    .word 0x17                                          # sizeof(__gdt) - 1
    .long __gdt


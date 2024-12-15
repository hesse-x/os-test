#include "update.h"

.text
.globl update_s
.globl __gdtdesc
.globl __gdt
.extern update_gdt
update_s:
my_asm_func:
    # 1. 保存旧的基址指针
    pushl %ebp
    movl %esp, %ebp

    # 2. 获取参数
    movl 8(%ebp), %eax

    # 3. 对指针进行操作
#    movl (%eax), %ebx
#    incl %ebx
#    movl %ebx, (%eax)

    pushl %eax
    pushl %eax
    call print_ptr
    popl %eax

    movl $__gdtdesc, %ebx
    pushl %ebx
    pushl %eax
    call update_gdt
    add $0x8,%esp

    # 4. 恢复栈
    movl %ebp, %esp
    popl %ebp

    # 5. 返回
    ret

.data
.align 4
__gdt:
    SEG_NULL
    SEG_ASM(0, - KERNBASE, 0xFFFFFFFF)      # code segment
    SEG_ASM(0, - KERNBASE, 0xFFFFFFFF)      # data segment
__gdtdesc:
    .word 0x17                              # sizeof(__gdt) - 1
    .long __gdt


# start.S - Higher-half kernel entry point
# 纯汇编实现，避免 -fPIE PIC thunk 覆盖 GRUB 传递的 multiboot 参数 (%eax/%ebx)
# _start 在物理地址运行，所有地址使用 VMA - VMA_BASE 计算物理地址

.section .text
.global _start
.type _start, @function

_start:
    # 1. 最先保存 Multiboot2 参数（GRUB: %eax=magic, %ebx=addr）
    #    后续地址计算会覆盖 %eax 和 %ebx，必须在此步保护
    movl %eax, %ecx
    movl %ebx, %edx

    # 2. 设置物理地址栈（stack_bottom VMA - VMA_BASE = 物理地址）
    movl $stack_bottom + 8192 - 0xC0000000, %esp

    # 3. 将参数压栈供 boot_main(int32_t magic_num, uintptr_t addr) 使用
    pushl %edx            # addr（2nd arg, C convention pushes right-to-left）
    pushl %ecx            # magic_num（1st arg）

    # 4. 计算 boot_main 物理地址并调用（VMA - VMA_BASE）
    movl $boot_main - 0xC0000000, %eax
    call *%eax

    # 5. 空闲循环（boot_main 不应返回）
    cli
1:  hlt
    jmp 1b

.size _start, . - _start
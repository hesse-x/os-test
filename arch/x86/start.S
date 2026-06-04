# start.S - Higher-half kernel entry point
# 纯汇编实现，避免 -fPIE PIC thunk 覆盖 GRUB 传递的 multiboot 参数 (%eax/%ebx)
# _start 在物理地址运行，所有地址使用 VMA - VMA_BASE 计算物理地址
# 流程: _start → enable_page(物理地址) → 栈平移 → kernel_main(虚拟地址)

.section .text
.global _start
.type _start, @function

_start:
    # 1. 保存 Multiboot2 参数到 callee-saved 寄存器（enable_page 会保留 esi/edi）
    movl %eax, %esi            # magic
    movl %ebx, %edi            # addr

    # 2. 设置物理地址栈
    movl $stack_bottom + 8192 - 0xC0000000, %esp

    # 3. 调用 enable_page（物理地址），启用分页
    movl $(enable_page - 0xC0000000), %eax
    call *%eax

    # 4. 栈平移到虚拟地址（ESP += VMA_BASE，指向同一物理内存）
    addl $0xC0000000, %esp

    # 5. 按C调用约定压栈：右参数先压
    pushl %edi                 # addr
    pushl %esi                 # magic

    # 6. 间接调用 kernel_main（虚拟地址）
    movl $kernel_main, %eax
    call *%eax

    # 7. 空闲循环（kernel_main 不应返回）
    cli
1:  hlt
    jmp 1b

.size _start, . - _start

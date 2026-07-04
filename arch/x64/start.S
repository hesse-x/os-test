# start.S - 内核入口（仅 x64）
# _start: 物理地址运行 → enable_paging → gdt_init → ljmp → _entry64 虚拟地址继续

.section .text
.code64
.global _start
.global _entry64
.type _start, @function

#define VMA_BASE 0xFFFFFFFF80000000  // mirrors boot/boot.h — keep in sync

_start:
    # rdi = boot_info* (物理地址, 由 EFI stub 传递)

    # 设置物理地址栈
    lea stack_bottom(%rip), %rax
    addq $8192, %rax
    movq %rax, %rsp

    # r12 = boot_info* 物理地址，跨 ljmp 保留
    movq %rdi, %r12

    # 建页表 + 加载 CR3
    call enable_paging

    # 配置 GDT + lgdt
    call gdt_init

    # 设段寄存器（GDT 已加载，当前仍在物理地址，identity map 可访问）
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    # 计算 _entry64 的虚拟地址：物理地址 + VMA_BASE
    lea _entry64(%rip), %rax
    addq $VMA_BASE, %rax

    # ljmp 到虚拟地址
    pushq $0x08
    pushq %rax
    lretq

.code64
_entry64:
    # --- 虚拟地址运行 ---

    # 切换到虚拟地址栈
    lea stack_bottom(%rip), %rsp
    addq $8192, %rsp

    # boot_info 物理地址 → 虚拟地址
    addq $VMA_BASE, %r12

    # 复制到 g_boot_info
    movq %r12, %rsi
    lea g_boot_info(%rip), %rdi
    movq $128, %rcx
    rep movsb

    # rdi = &g_boot_info
    lea g_boot_info(%rip), %rdi

    call kernel_main

    cli
1:  hlt
    jmp 1b

.size _start, . - _start

    .section .note.GNU-stack,"",@progbits

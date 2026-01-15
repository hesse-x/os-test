; 符合Multiboot v1规范的汇编入口
[BITS 32] ; GRUB2切换到保护模式后，默认以32位模式执行（后续可切换到64位）
[GLOBAL boot] ; 导出内核入口符号，供链接器使用
[EXTERN kernel_main] ; 声明C语言内核入口函数，后续链接时关联

; 定义Multiboot头部（GRUB2识别的标记）
MODULEALIGN equ  1<<0
MEMINFO     equ  1<<1
FLAGS       equ  MODULEALIGN | MEMINFO
MAGIC       equ  0x1BADB002 ; Multiboot v1魔数
CHECKSUM    equ -(MAGIC + FLAGS)

; Multiboot头部数据（必须放在镜像开头，让GRUB2识别）
section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM

; 定义堆栈区域（内核运行需要堆栈，分配16KB栈空间）
section .bss
align 16
stack_bottom:
resb 16384 ; 分配16KB字节作为堆栈
stack_top:

; 内核入口函数（GRUB2转交控制权后执行的入口）
section .text
boot:
    ; 初始化堆栈指针（指向栈顶，x86架构栈向低地址增长）
    mov esp, stack_top

    ; 调用C语言内核入口函数
    call kernel_main

    ; 若kernel_main返回，进入无限循环（防止程序退出）
    cli ; 关闭中断
.hang:
    hlt ; 暂停CPU，降低功耗
    jmp .hang

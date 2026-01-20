/* linker.ld - 32位x86链接脚本 */
OUTPUT_FORMAT("elf32-i386")  /* 输出32位ELF格式 */
OUTPUT_ARCH(i386)            /* 目标架构为i386 */

ENTRY(_start)  /* 入口点为_start符号 */

/* 内存布局：假设内核加载到物理地址0x100000（1MB）处 */
PHDRS {
    kernel PT_LOAD FLAGS(5);  /* 可执行+可加载段 */
}

SECTIONS {
    /* 代码段和multiboot段加载到0x100000 */
    . = 0x100000;

    /* Multiboot头部段 */
    .multiboot : {
        *(.multiboot)
    } :kernel

    /* 代码段 */
    .text : {
        *(.text)
    } :kernel

    /* 已初始化数据段 */
    .data : {
        *(.data)
    } :kernel

    /* 未初始化数据段（BSS） */
    .bss : {
        *(.bss)
    } :kernel
}

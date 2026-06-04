/* linker.ld - 32位x86链接脚本 (Higher-half kernel) */
OUTPUT_FORMAT("elf32-i386")  /* 输出32位ELF格式 */
OUTPUT_ARCH(i386)            /* 目标架构为i386 */

ENTRY(_start)  /* 入口点为_start符号 */

. = 0xC0100000;
SECTIONS {
    /* Multiboot头部段 */
    .multiboot : AT(ADDR(.multiboot) - 0xC0000000) {
        *(.multiboot)
    }

    .stack : AT(ADDR(.stack) - 0xC0000000) {
        *(.stack)
    }

    /* 代码段 */
    .text : AT(ADDR(.text) - 0xC0000000) {
        *(.text)
        *(.text.*)
    }

    .rodata : AT(ADDR(.rodata) - 0xC0000000) {
        *(.rodata)
        *(.rodata.*)
    }

    /* 已初始化数据段 */
    .data : AT(ADDR(.data) - 0xC0000000) {
        *(.data)
        *(.data.*)
        *(.data.rel.ro.local)
        *(.data.rel.ro)
        *(.data.rel.ro.*)
    }

    /* GOT段 (-fPIE生成) */
    .got : AT(ADDR(.got) - 0xC0000000) {
        *(.got)
        *(.got.plt)
    }

    . = ALIGN(4096);
    /* 未初始化数据段（BSS） - 4KB对齐供PD/PT使用 */
    .bss (NOLOAD) : AT(ADDR(.bss) - 0xC0000000) {
        *(.bss)
        *(COMMON)
    }

    kernel_end = .;

    /* 丢弃编译器自动生成的冗余段（关键！避免乱序） */
    /DISCARD/ : {
        *(.eh_frame)
        *(.note.gnu.property)
        *(.note.gnu.build-id)
        *(.comment)
    }
}

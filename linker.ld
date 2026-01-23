/* linker.ld - 32位x86链接脚本 */
OUTPUT_FORMAT("elf32-i386")  /* 输出32位ELF格式 */
OUTPUT_ARCH(i386)            /* 目标架构为i386 */

ENTRY(_start)  /* 入口点为_start符号 */

. = 0xc0100000;
SECTIONS {
    /* Multiboot头部段 */
    .multiboot : AT(0x100000) {
        *(.multiboot)
    }

    .stack : AT(LOADADDR(.multiboot) + SIZEOF(.multiboot)) {
        *(.stack)
    }

    /* 代码段 */
    .text : AT(LOADADDR(.stack) + SIZEOF(.stack)) {
        *(.text)
    }

    .rodata : AT(LOADADDR(.text) + SIZEOF(.text)) {
        *(.rodata)
        *(.rodata.*)
    }

    /* 已初始化数据段 */
    .data : AT(LOADADDR(.rodata) + SIZEOF(.rodata)) {
        *(.data)
    }

    /* 未初始化数据段（BSS） */
    .bss : {
        *(.bss)
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

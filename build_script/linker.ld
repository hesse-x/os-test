/* linker.ld - 64位x86-64链接脚本 (Higher-half kernel, EFI stub boot) */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)

ENTRY(_start)

. = 0xFFFFFFFF80100000;
SECTIONS {
    /* 代码段 */
    .text : AT(ADDR(.text) - 0xFFFFFFFF80000000) {
        *(.text)
        *(.text.*)
    } :text

    .rodata : AT(ADDR(.rodata) - 0xFFFFFFFF80000000) {
        *(.rodata)
        *(.rodata.*)
    } :rodata

    /* 已初始化数据段 */
    .data : AT(ADDR(.data) - 0xFFFFFFFF80000000) {
        *(.data)
        *(.data.*)
        *(.data.rel.ro.local)
        *(.data.rel.ro)
        *(.data.rel.ro.*)
    } :data

    /* GOT段 */
    .got : AT(ADDR(.got) - 0xFFFFFFFF80000000) {
        *(.got)
        *(.got.plt)
    } :data

    /* KASAN global variable redzone metadata */
    __start_kasan_globals = .;
    .kasan_globals : AT(ADDR(.kasan_globals) - 0xFFFFFFFF80000000) {
        KEEP(*(__kasan_globals))
    } :data
    __stop_kasan_globals = .;

    . = ALIGN(4096);
    /* 未初始化数据段（BSS） - 4KB对齐供页表使用 */
    .bss (NOLOAD) : AT(ADDR(.bss) - 0xFFFFFFFF80000000) {
        *(.bss)
        *(COMMON)
    } :data

    kernel_end = .;

    /* 丢弃编译器自动生成的冗余段 */
    /DISCARD/ : {
        *(.eh_frame)
        *(.note.gnu.property)
        *(.note.gnu.build-id)
        *(.comment)
    }
}

/* 显式分段：代码 R+X、只读 R、数据 R+W，避免单个 LOAD 段 RWX 权限告警，
   并为分页启用后的页级 W^X/NX 保护铺路 */
PHDRS {
    text   PT_LOAD FLAGS(5);   /* R+X */
    rodata PT_LOAD FLAGS(4);   /* R   */
    data   PT_LOAD FLAGS(6);   /* R+W */
}

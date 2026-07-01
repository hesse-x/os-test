/* 用户态 ELF 链接脚本（静态主 ELF）
 * 基址 0x400000，含 TLS 段符号 + .init_array/.fini_array
 * 阶段 0 只需 TLS 符号；.init_array/.fini_array 在阶段 2b+3 启用
 */
ENTRY(_start)

SECTIONS
{
    . = 0x400000;

    .text : {
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
        *(.got)
    }

    /* TLS 初始化数据段 */
    .tdata : {
        __tls_template_start = .;
        *(.tdata .tdata.*)
        __tls_template_end = .;
    }

    /* TLS BSS 段（运行时清零） */
    .tbss : {
        __tls_tbss_start = .;
        *(.tbss .tbss.*)
        __tls_tbss_end = .;
    }

    /* TLS 大小符号（链接器算后写入） */
    __tls_tdata_size = SIZEOF(.tdata);
    __tls_tbss_size = SIZEOF(.tbss);
    __tls_align = ALIGNOF(.tdata);

    .init_array : {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    }

    .fini_array : {
        __fini_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP(*(.fini_array))
        __fini_array_end = .;
    }

    .bss : {
        *(.bss .bss.*)
    }

    /DISCARD/ : {
        *(.eh_frame)
        *(.note.gnu.property)
        *(.note.gnu.build-id)
        *(.comment)
    }
}

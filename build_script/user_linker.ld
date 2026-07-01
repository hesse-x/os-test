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
    __tdata_start = __tls_template_start;
    __tdata_end = __tls_template_end;
    __tdata_size = SIZEOF(.tdata);
    __tdata_align = ALIGNOF(.tdata);

    /* TLS BSS 段（运行时清零） */
    .tbss (NOLOAD) : {
        __tbss_start = .;
        *(.tbss .tbss.*)
        __tbss_end = .;
    }
    __tbss_size = SIZEOF(.tbss);
    __tbss_align = ALIGNOF(.tbss);

    __tls_align = MAX(__tdata_align, __tbss_align);

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
        *(.note.gnu.property)
        *(.note.gnu.build-id)
        *(.comment)
    }
}

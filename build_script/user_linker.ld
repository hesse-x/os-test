/* Userspace ELF linker script (static main ELF)
 * Base 0x400000, includes TLS section symbols + .init_array/.fini_array
 * Phase 0 only needs TLS symbols; .init_array/.fini_array are enabled in phase 2b+3
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

    /* TLS initialization data section */
    .tdata : {
        __tls_template_start = .;
        *(.tdata .tdata.*)
        __tls_template_end = .;
    }
    __tdata_start = __tls_template_start;
    __tdata_end = __tls_template_end;
    __tdata_size = SIZEOF(.tdata);
    __tdata_align = ALIGNOF(.tdata);

    /* TLS BSS section (zeroed at runtime) */
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

/* Userspace ELF linker script (static main ELF)
 * Base 0x400000, includes TLS section symbols + .init_array/.fini_array
 * Phase 0 only needs TLS symbols; .init_array/.fini_array are enabled in phase 2b+3
 *
 * Explicit PHDRS: split code/ro/data into separate PT_LOAD segments with
 * distinct permissions (R+X / R / R+W), so no single LOAD segment is RWX.
 * This silences the binutils "LOAD segment with RWX permissions" warning and
 * lets the ELF loader apply W^X page protection per segment. Each segment is
 * page-aligned (ld aligns every PT_LOAD to its p_align=0x1000); without
 * alignment a .text and .data sharing one 4K page would have one page's
 * p_flags overwrite the other in elf_loader's map_page (same bug class as the
 * kernel linker.ld comment warns about). TLS keeps its custom __tls_template_*
 * symbols rather than a PT_TLS program header.
 */
ENTRY(_start)

SECTIONS
{
    . = 0x400000;

    /* Each segment is page-aligned at its start AND end so adjacent segments
     * never share a 4K page. The ELF loader maps each PT_LOAD page-independently
     * with that segment's p_flags; if .text and .rodata shared a page, the
     * later-mapped rodata (R) would overwrite the page's earlier .text (R+X)
     * mapping and its file contents, corrupting the code tail and crashing on
     * fetch (err=0x15, observed RIP inside the shared page). Same hazard the
     * kernel linker.ld comment warns about. */
    . = ALIGN(0x1000);
    .text : {
        *(.text .text.*)
    } :text
    . = ALIGN(0x1000);

    .rodata : {
        *(.rodata .rodata.*)
        *(.eh_frame .eh_frame.*)
    } :rodata
    . = ALIGN(0x1000);

    .data : {
        *(.data .data.*)
        *(.got)
        *(.got.plt)
    } :data

    /* TLS initialization data section */
    .tdata : {
        __tls_template_start = .;
        *(.tdata .tdata.*)
        __tls_template_end = .;
    } :data
    __tdata_start = __tls_template_start;
    __tdata_end = __tls_template_end;
    __tdata_size = SIZEOF(.tdata);
    __tdata_align = ALIGNOF(.tdata);

    /* TLS BSS section (zeroed at runtime) */
    .tbss (NOLOAD) : {
        __tbss_start = .;
        *(.tbss .tbss.*)
        __tbss_end = .;
    } :data
    __tbss_size = SIZEOF(.tbss);
    __tbss_align = ALIGNOF(.tbss);

    __tls_align = MAX(__tdata_align, __tbss_align);

    .init_array : {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    } :data

    .fini_array : {
        __fini_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP(*(.fini_array))
        __fini_array_end = .;
    } :data

    .bss : {
        *(.bss .bss.*)
    } :data

    /DISCARD/ : {
        *(.note.gnu.property)
        *(.note.gnu.build-id)
        *(.comment)
    }
}

/* Explicit segmentation: code R+X, read-only R, data R+W — avoids a single
 * RWX LOAD segment (binutils warning) and enables page-level W^X protection.
 * Matches the kernel linker.ld scheme. */
PHDRS {
    text   PT_LOAD FLAGS(5);   /* R+X */
    rodata PT_LOAD FLAGS(4);   /* R   */
    data   PT_LOAD FLAGS(6);   /* R+W */
}

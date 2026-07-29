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

    /* The first PT_LOAD must cover file offset 0 (the ELF header + PHDR table)
     * so that AT_PHDR = base + e_phoff lands in mapped memory. musl's __init_tls
     * (and any dynamic ld.so) dereferences aux[AT_PHDR] to walk the program
     * headers and find PT_TLS; if the header page is excluded from every
     * PT_LOAD (as it was when .text was forced to a page boundary at 0x400000,
     * making the first LOAD start at file offset 0x1000), AT_PHDR points at an
     * unmapped page and the process SIGSEGVs on the first phdr->p_type read.
     *
     * Two things together put the headers in the first PT_LOAD:
     *  - .text starts at 0x400000 + SIZEOF_HEADERS, leaving room for the ELF
     *    header + PHDR table at the front of the text segment's vaddr range;
     *  - the text PHDR is declared `FILEHDR PHDRS` (see PHDRS below), which
     *    tells ld to extend that segment's p_offset/p_vaddr back to 0/0x400000
     *    so it actually carries the headers. With an explicit PHDRS block, ld
     *    does NOT auto-cover the headers (unlike the default script), so
     *    FILEHDR PHDRS is mandatory here.
     * The header page thus shares the first R+X PT_LOAD with .text — the
     * standard glibc static layout. The page-alignment rule below still applies
     * BETWEEN segments of different permissions (text/rodata/data), so they
     * never share a 4K page. */
    . = 0x400000 + SIZEOF_HEADERS;
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

    /* TLS initialization data section.
     * .tdata stays in the :data PT_LOAD (it must be loaded from the file to
     * initialize the TLS block), AND is also described by a dedicated PT_TLS
     * program header (see PHDRS below). musl's __init_tls scans the auxv for
     * PT_TLS (AT_PHDR/AT_PHNUM/AT_PHENT) to locate the TLS init-image and its
     * size, then mmaps the per-thread TLS block and copies the template in.
     * Without a PT_TLS segment __init_tls sees tls_size==0 and skips TLS setup
     * — fine for programs with no TLS, but musl's struct pthread/errno/etc. are
     * themselves TLS, so __errno_location (mov %fs:0 → struct pthread) needs a
     * real TLS block. The PT_TLS segment's p_filesz = .tdata size (the
     * init-image), p_memsz = .tdata + .tbss (the full zero-initialized block). */
    .tdata : {
        __tls_template_start = .;
        *(.tdata .tdata.*)
        __tls_template_end = .;
    } :data :tls
    __tdata_start = __tls_template_start;
    __tdata_end = __tls_template_end;
    __tdata_size = SIZEOF(.tdata);
    __tdata_align = ALIGNOF(.tdata);

    /* TLS BSS section (zeroed at runtime). NOLOAD — not backed by file bytes;
     * it only extends the PT_TLS p_memsz past p_filesz. Assigned to :tls so the
     * PT_TLS segment covers both .tdata (filesz) and .tbss (memsz tail). */
    .tbss (NOLOAD) : {
        __tbss_start = .;
        *(.tbss .tbss.*)
        __tbss_end = .;
    } :tls
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
 * Matches the kernel linker.ld scheme. The tls PT_TLS segment describes the
 * TLS initialization image (.tdata filesz + .tbss memsz tail) for musl's
 * __init_tls; it carries no p_flags requirement (FLAGS(4) = R). */
PHDRS {
    text   PT_LOAD FILEHDR PHDRS FLAGS(5);   /* R+X — also covers ELF header + PHDR table (see SIZEOF_HEADERS comment above) */
    rodata PT_LOAD FLAGS(4);   /* R   */
    data   PT_LOAD FLAGS(6);   /* R+W */
    tls    PT_TLS FLAGS(4);    /* R   */
}

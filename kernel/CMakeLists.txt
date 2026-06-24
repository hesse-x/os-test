add_subdirectory(mem)

add_kernel_object(kernel_obj
    SOURCES kernel.c serial.c trap.c proc.c ahci.c elf_loader.c acpi.c pci.c xhci.c socket.c
            inode.c page_cache.c blk_dev.c fat32.c vfs.c devtmpfs.c display.c
)

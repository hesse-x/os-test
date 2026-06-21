add_subdirectory(mem)

add_kernel_object(kernel_obj
    SOURCES kernel.c serial.c trap.c proc.c fb.c ahci.c elf_loader.c acpi.c pci.c xhci.c socket.c
)

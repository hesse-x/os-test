add_kernel_object(bsd_obj
    SOURCES init.c syscall.c proc.c proc_create.c signal.c
            vfs.c fat32.c inode.c page_cache.c devtmpfs.c
            socket.c pty.c elf_loader.c
)

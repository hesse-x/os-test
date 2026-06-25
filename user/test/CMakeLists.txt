# user/test/ — Unity-based test ELF targets

# Test runner (no Unity needed)
add_user_elf(test_runner C SOURCES test_runner.c LINK_LIBS c)

# Layer 1: unistd (pipe/read/write/close)
add_user_elf(pipe C SOURCES test_pipe.c LINK_LIBS c unity)

# Layer 2: fcntl (open/close/dup2/fcntl/lseek)
add_user_elf(fcntl C SOURCES test_fcntl.c LINK_LIBS c unity)

# Layer 3: string / malloc / stdio
add_user_elf(string C SOURCES test_string.c LINK_LIBS c unity)
add_user_elf(malloc C SOURCES test_malloc.c LINK_LIBS c unity)
add_user_elf(stdio C SOURCES test_stdio.c LINK_LIBS c unity)

# Layer 4: sys/mman
add_user_elf(mmap C SOURCES test_mmap.c LINK_LIBS c unity)

# Layer 5: IPC
add_user_elf(ipc C SOURCES test_ipc.c LINK_LIBS c unity)

# Layer 6: socket
add_user_elf(socket C SOURCES test_socket.c LINK_LIBS c unity)

# Layer 7: process
add_user_elf(process C SOURCES test_process.c LINK_LIBS c unity)

# Layer 8: signal
add_user_elf(signal C SOURCES test_signal.c LINK_LIBS c unity)

# Layer 9: poll
add_user_elf(poll C SOURCES test_poll.c LINK_LIBS c unity)

# Layer 10: PCI/device
add_user_elf(pci C SOURCES test_pci.c LINK_LIBS c unity)

# Layer 11: ioctl/fstat/isatty
add_user_elf(ioctl C SOURCES test_ioctl.c LINK_LIBS c unity)

# Layer 12: device VFS unified path
add_user_elf(dev_vfs C SOURCES test_dev_vfs.c LINK_LIBS c unity)

# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

add_kernel_object(bsd_obj
    SOURCES init.c syscall.c proc.c proc_create.c signal.c
            vfs.c fat32.c inode.c page_cache.c devtmpfs.c mount.c sysfs.c
            socket.c pty.c elf_loader.c futex.c
            file_wq.c file_poll.c eventpoll.c eventfd.c timerfd.c signalfd.c
            netlink.c fops.c
            ring.c
)

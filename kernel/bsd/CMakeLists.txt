# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

add_kernel_object(bsd_obj
    SOURCES init.c syscall.c proc.c proc_create.c signal.c
            vfs.c fat32.c inode.c page_cache.c devtmpfs.c mount.c sysfs.c tmpfs.c
            socket.c pty.c elf_loader.c futex.c
            file_wq.c file_poll.c file_lock.c eventpoll.c eventfd.c timerfd.c signalfd.c
            netlink.c fops.c ipcfd.c random.c file_fault.c
            evdev_broker.c kfifo.c
)
# 层间依赖图（reface_cmake.md §4.1/§3.5）：bsd_obj 依赖 xcore_obj（传递 xcore_mem/arch_x64）。
target_link_libraries(bsd_obj PUBLIC xcore_obj)

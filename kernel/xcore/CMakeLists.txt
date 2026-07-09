# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

add_subdirectory(mem)

add_kernel_object(xcore_obj
    SOURCES init.c trap.c sched.c ipc.c
            log.c rcu.c acpi.c ${CMAKE_SOURCE_DIR}/utils/kvformat.c
            wait_queue.c rbtree.c
)

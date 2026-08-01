# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

add_subdirectory(mem)

add_kernel_object(xcore_obj
    SOURCES init.c trap.c sched.c ipc.c
            log.c rcu.c acpi.c ${CMAKE_SOURCE_DIR}/utils/kvformat.c
            wait_queue.c rbtree.c random.c
)
# Layer dependency graph: xcore_obj -> xcore_mem + arch_x64.
# PUBLIC propagates usage requirements (include/flags) to upstream bsd/driver/kernel
# and expresses the "linking xcore_obj needs xcore_mem/arch_x64 objects first" edge.
target_link_libraries(xcore_obj PUBLIC xcore_mem arch_x64)

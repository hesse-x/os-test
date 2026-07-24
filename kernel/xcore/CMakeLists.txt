# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

add_subdirectory(mem)

add_kernel_object(xcore_obj
    SOURCES init.c trap.c sched.c ipc.c
            log.c rcu.c acpi.c ${CMAKE_SOURCE_DIR}/utils/kvformat.c
            wait_queue.c rbtree.c random.c
)
# 层间依赖图（reface_cmake.md §4.1/§3.5）：xcore_obj 依赖 xcore_mem + arch_x64，
# 经 PUBLIC 传递 usage requirement（include/flag）给上游 bsd/driver/kernel，
# 同时表达"链接 xcore_obj 需先有 xcore_mem/arch_x64 对象"的依赖边。
target_link_libraries(xcore_obj PUBLIC xcore_mem arch_x64)

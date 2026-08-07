# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

add_subdirectory(mem)

add_kernel_object(xcore_obj
    SOURCES init.c trap.c sched.c ipc.c
            log.c rcu.c acpi.c ${CMAKE_SOURCE_DIR}/utils/kvformat.c
            wait_queue.c mutex.c completion.c kthread.c workqueue.c selftest.c rbtree.c random.c perf/phase.c perf/core.c perf/sample.c
            perf/unwind.c perf/pmu.c perf/event.c perf/counter.c
)
if(SANITIZE)
    # printk is used by init_mem/acpi before kasan_init has mapped shadow.
    set_source_files_properties(log.c ${CMAKE_SOURCE_DIR}/utils/kvformat.c
        PROPERTIES COMPILE_OPTIONS "-fno-sanitize=kernel-address")
endif()
# Layer dependency graph: xcore_obj -> xcore_mem + arch_x64.
# PUBLIC propagates usage requirements (include/flags) to upstream bsd/driver/kernel
# and expresses the "linking xcore_obj needs xcore_mem/arch_x64 objects first" edge.
target_link_libraries(xcore_obj PUBLIC xcore_mem arch_x64)

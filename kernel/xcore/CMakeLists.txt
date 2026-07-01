add_subdirectory(mem)

add_kernel_object(xcore_obj
    SOURCES init.c trap.c sched.c ipc.c perf.c
            log.c rcu.c acpi.c ${CMAKE_SOURCE_DIR}/common/kvformat.c
)

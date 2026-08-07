# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

add_subdirectory(xcore)
add_subdirectory(bsd)
add_subdirectory(driver)

add_kernel_object(kernel_obj
    SOURCES kernel.c perf_counter_provider.c
)
# Layer dependency graph: kernel_obj -> driver_obj (transitively bsd->xcore->mem->arch).
# myos.elf aggregates all six OBJECT libraries via target_link_libraries(kernel_obj).
target_link_libraries(kernel_obj PUBLIC driver_obj)

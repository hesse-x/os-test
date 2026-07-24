# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

add_subdirectory(xcore)
add_subdirectory(bsd)
add_subdirectory(driver)

add_kernel_object(kernel_obj
    SOURCES kernel.c
)
# 层间依赖图（reface_cmake.md §4.1/§3.5）：kernel_obj 依赖 driver_obj（传递 bsd→xcore→mem→arch）。
# myos.elf 经 target_link_libraries(kernel_obj) 即聚合全部六个 OBJECT 库。
target_link_libraries(kernel_obj PUBLIC driver_obj)

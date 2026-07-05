# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

add_subdirectory(xcore)
add_subdirectory(bsd)
add_subdirectory(driver)

add_kernel_object(kernel_obj
    SOURCES kernel.c
)

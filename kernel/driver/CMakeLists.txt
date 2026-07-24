# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

add_kernel_object(driver_obj
    SOURCES init.c registry.c
            ahci.c xhci.c pci.c serial.c blk_dev.c
            virtio_pci.c virtio_ring.c virtio_gpu.c
)
# DRM UAPI headers — upstream headers in third_party/drm/include/drm/ are
# included as "drm/drm.h" by virtio_gpu.c (and drm_internal.h).
target_include_directories(driver_obj PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/drm/include)
# 层间依赖图（reface_cmake.md §4.1/§3.5）：driver_obj 依赖 bsd_obj（传递 xcore_obj→mem→arch）。
target_link_libraries(driver_obj PUBLIC bsd_obj)

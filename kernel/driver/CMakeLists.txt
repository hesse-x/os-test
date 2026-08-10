# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

add_kernel_object(driver_obj
    SOURCES init.c registry.c dma_buf.c dma_resv.c udmabuf.c
            ahci.c xhci.c pci.c serial.c blk_dev.c
            virtio_pci.c virtio_ring.c virtio_gpu.c
            intel/i915_probe.c intel/i915_uncore.c intel/i915_stolen.c
            intel/i915_irq.c
            drm/drm_core.c drm/drm_fence.c drm/drm_mock.c
)
if(TEST)
    target_sources(driver_obj PRIVATE intel/i915_mock.c)
endif()
if(SANITIZE)
    # The polled serial console is part of the pre-KASAN diagnostic path.
    set_source_files_properties(serial.c
        PROPERTIES COMPILE_OPTIONS "-fno-sanitize=kernel-address")
endif()
# DRM UAPI headers — upstream headers in third_party/drm/include/drm/ are
# included as "drm/drm.h" by virtio_gpu.c (and drm_internal.h).
target_include_directories(driver_obj PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/drm/include)
# Layer dependency graph: driver_obj -> bsd_obj (transitively xcore_obj->mem->arch).
target_link_libraries(driver_obj PUBLIC bsd_obj)

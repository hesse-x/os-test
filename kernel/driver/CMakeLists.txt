# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

add_kernel_object(driver_obj
    SOURCES init.c registry.c
            ahci.c xhci.c pci.c serial.c blk_dev.c
            virtio_pci.c virtio_ring.c virtio_gpu.c
            intel/i915_probe.c
            drm/drm_core.c drm/drm_fence.c drm/drm_mock.c
)
target_compile_definitions(driver_obj PRIVATE
    I915_PROBE_STAGE=${I915_PROBE_STAGE_VALUE})

# Production stage images do not carry hardware helpers that their probe path
# cannot reach. TEST keeps the component mocks available at every stage.
if(TEST)
    target_sources(driver_obj PRIVATE
        intel/i915_uncore.c intel/i915_stolen.c)
elseif(I915_PROBE_STAGE_VALUE GREATER_EQUAL 2)
    target_sources(driver_obj PRIVATE
        intel/i915_uncore.c intel/i915_stolen.c)
endif()
if(TEST)
    target_sources(driver_obj PRIVATE intel/i915_irq.c)
elseif(I915_PROBE_STAGE_VALUE GREATER_EQUAL 4)
    target_sources(driver_obj PRIVATE intel/i915_irq.c)
endif()
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

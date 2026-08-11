# Vendored wlroots Vulkan renderer

This directory contains the Vulkan renderer implementation copied from
wlroots commit `d783533489e1f75d6886c2ab5c5960090ef268f8` (0.20.2 development
snapshot). The original license headers and internal symbol names are retained
to keep future source comparisons straightforward.

The `support/` directory contains the wlroots internal helper translation
units required by the renderer. These helpers are not exported from the
wlroots shared library when its Vulkan renderer is disabled.

Local compatibility changes carried in the copied source:

- `vulkan.c` permits the sole CPU Vulkan device when
  `VK_EXT_physical_device_drm` is unavailable. The compositor's PRIME probe is
  the final compatibility gate.
- `renderer.c` duplicates the compositor DRM fd when that CPU device has no
  discoverable render node.

wlroots itself is built without its Vulkan renderer. `os_vk_renderer.c`
provides the repository-owned public entry point used by the compositor.

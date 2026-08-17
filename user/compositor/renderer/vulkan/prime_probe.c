/*
 * Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#include "prime_probe.h"

#include <drm_fourcc.h>
#include <inttypes.h>
#include <linux/dma-buf.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/log.h>
#include <xf86drmMode.h>

// Exported by wlroots 0.20.2 but intentionally omitted from its public API.
const struct wlr_drm_format_set *
wlr_renderer_get_render_formats(struct wlr_renderer *renderer);

static bool has_linear_format(const struct wlr_drm_format_set *formats,
                              uint32_t format) {
  return formats != NULL &&
         wlr_drm_format_set_has(formats, format, DRM_FORMAT_MOD_LINEAR);
}

static bool get_probe_size(int drm_fd, int *width, int *height) {
  drmModeRes *resources = drmModeGetResources(drm_fd);
  if (resources == NULL) {
    return false;
  }
  bool found = false;
  for (int i = 0; i < resources->count_connectors && !found; ++i) {
    drmModeConnector *connector =
        drmModeGetConnector(drm_fd, resources->connectors[i]);
    if (connector != NULL && connector->connection == DRM_MODE_CONNECTED &&
        connector->count_modes > 0) {
      int mode = 0;
      for (int j = 0; j < connector->count_modes; ++j) {
        if (connector->modes[j].type & DRM_MODE_TYPE_PREFERRED) {
          mode = j;
          break;
        }
      }
      *width = connector->modes[mode].hdisplay;
      *height = connector->modes[mode].vdisplay;
      found = *width > 0 && *height > 0;
    }
    drmModeFreeConnector(connector);
  }
  drmModeFreeResources(resources);
  return found;
}

bool os_vulkan_prime_probe(struct wlr_renderer *renderer,
                           struct wlr_allocator *allocator, int drm_fd) {
  const struct wlr_drm_format_set *formats =
      wlr_renderer_get_render_formats(renderer);
  if (!has_linear_format(formats, DRM_FORMAT_XRGB8888) ||
      !has_linear_format(formats, DRM_FORMAT_ARGB8888)) {
    wlr_log(WLR_ERROR,
            "compositor_vulkan_prime_smoke: LINEAR XR24/AR24 unavailable");
    return false;
  }

  uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
  struct wlr_drm_format format = {
      .format = DRM_FORMAT_XRGB8888,
      .len = 1,
      .capacity = 1,
      .modifiers = &modifier,
  };
  int width = 0, height = 0;
  if (!get_probe_size(drm_fd, &width, &height)) {
    wlr_log(WLR_ERROR, "compositor_vulkan_prime_smoke: no connected KMS mode");
    return false;
  }
  struct wlr_buffer *buffer =
      wlr_allocator_create_buffer(allocator, width, height, &format);
  if (buffer == NULL) {
    wlr_log(WLR_ERROR,
            "compositor_vulkan_prime_smoke: dma-buf allocation failed");
    return false;
  }

  bool ok = false;
  struct wlr_dmabuf_attributes dmabuf;
  if (!wlr_buffer_get_dmabuf(buffer, &dmabuf) || dmabuf.n_planes != 1 ||
      dmabuf.format != DRM_FORMAT_XRGB8888 ||
      dmabuf.modifier != DRM_FORMAT_MOD_LINEAR || dmabuf.offset[0] != 0 ||
      dmabuf.stride[0] < (uint32_t)width * 4) {
    wlr_log(WLR_ERROR, "compositor_vulkan_prime_smoke: invalid PRIME layout");
    goto out;
  }

  struct wlr_render_pass *pass =
      wlr_renderer_begin_buffer_pass(renderer, buffer, NULL);
  if (pass == NULL) {
    wlr_log(WLR_ERROR, "compositor_vulkan_prime_smoke: VkImage import failed");
    goto out;
  }
  struct wlr_render_rect_options rect = {
      .box = {.x = 0, .y = 0, .width = width, .height = height},
      .color = {.r = 1.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f},
      .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
  };
  wlr_render_pass_add_rect(pass, &rect);
  if (!wlr_render_pass_submit(pass)) {
    wlr_log(WLR_ERROR,
            "compositor_vulkan_prime_smoke: queue submit/fence failed");
    goto out;
  }

  size_t stride = dmabuf.stride[0];
  size_t map_size = stride * (size_t)height;
  void *data = mmap(NULL, map_size, PROT_READ, MAP_SHARED, dmabuf.fd[0], 0);
  if (data == MAP_FAILED) {
    wlr_log(WLR_ERROR,
            "compositor_vulkan_prime_smoke: CPU readback mapping failed");
    goto out;
  }
  struct dma_buf_sync sync = {
      .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ,
  };
  if (ioctl(dmabuf.fd[0], DMA_BUF_IOCTL_SYNC, &sync) != 0) {
    wlr_log(WLR_ERROR,
            "compositor_vulkan_prime_smoke: CPU readback sync failed");
    munmap(data, map_size);
    goto out;
  }
  const uint32_t pixel = *(const uint32_t *)data;
  sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
  int sync_end = ioctl(dmabuf.fd[0], DMA_BUF_IOCTL_SYNC, &sync);
  munmap(data, map_size);
  if (sync_end != 0 || (pixel & 0x00FFFFFFu) != 0x00FF0000u) {
    wlr_log(WLR_ERROR,
            "compositor_vulkan_prime_smoke: readback mismatch "
            "stride=%zu pixel=%08" PRIx32,
            stride, pixel);
    goto out;
  }

  wlr_log(WLR_INFO,
          "[PASS] compositor_vulkan_prime_smoke size=%dx%d stride=%zu "
          "modifier=LINEAR",
          width, height, stride);
  ok = true;

out:
  wlr_buffer_drop(buffer);
  return ok;
}

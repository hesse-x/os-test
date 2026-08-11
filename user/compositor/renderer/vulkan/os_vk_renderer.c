/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#include "os_vk_renderer.h"

#include <wlr/render/vulkan.h>

struct wlr_renderer *os_vk_renderer_create_with_drm_fd(int drm_fd) {
  return wlr_vk_renderer_create_with_drm_fd(drm_fd);
}

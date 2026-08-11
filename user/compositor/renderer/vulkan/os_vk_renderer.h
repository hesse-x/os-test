/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OS_COMPOSITOR_VULKAN_RENDERER_H
#define OS_COMPOSITOR_VULKAN_RENDERER_H

struct wlr_renderer;

struct wlr_renderer *os_vk_renderer_create_with_drm_fd(int drm_fd);

#endif

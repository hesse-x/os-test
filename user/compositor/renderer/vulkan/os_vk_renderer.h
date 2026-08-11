/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OS_COMPOSITOR_VULKAN_RENDERER_H
#define OS_COMPOSITOR_VULKAN_RENDERER_H

#include <stdbool.h>
#include <stdint.h>

struct wlr_renderer;
struct wlr_render_pass;
struct wlr_texture;
struct os_mesh;

struct os_vk_rounded_rect {
  float x, y, width, height;
  float radius, border_width, rotation_radians;
  float fill[4];
  float border[4];
};

struct os_vk_backdrop_blur {
  int32_t x, y, width, height;
  uint32_t radius;
  uint32_t downsample;
  float corner_radius;
  float tint[4];
  float opacity;
};

struct wlr_renderer *os_vk_renderer_create_with_drm_fd(int drm_fd);
bool os_vk_pass_add_rounded_rect(struct wlr_render_pass *pass,
                                 const struct os_vk_rounded_rect *rect);
bool os_vk_pass_add_mesh(struct wlr_render_pass *pass,
                         const struct os_mesh *mesh,
                         struct wlr_texture *texture, float opacity);
bool os_vk_pass_add_backdrop_blur(struct wlr_render_pass *pass,
                                  const struct os_vk_backdrop_blur *blur,
                                  struct wlr_texture *backdrop);

#endif

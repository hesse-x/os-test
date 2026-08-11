/* Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#ifndef OS_COMPOSITOR_VISUAL_H
#define OS_COMPOSITOR_VISUAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OS_SHADOW_CACHE_CAPACITY 32u

enum os_pixel_alpha {
  OS_PIXEL_ALPHA_PREMULTIPLIED,
  OS_PIXEL_ALPHA_OPAQUE,
};

struct os_linear_color {
  float r, g, b, a;
};

struct os_visual_rect {
  float x, y, width, height;
  float radius;
  float border_width;
  float rotation_radians;
};

struct os_shadow_key {
  uint32_t width, height;
  uint32_t radius_q8, blur_q8;
  int32_t offset_x_q8, offset_y_q8;
  uint32_t color_rgba8;
};

struct os_shadow_entry {
  struct os_shadow_key key;
  uint64_t resource_id;
  uint64_t last_used;
  bool occupied;
};

struct os_shadow_cache {
  struct os_shadow_entry entries[OS_SHADOW_CACHE_CAPACITY];
  uint64_t clock;
};

float os_srgb_decode(float value);
float os_srgb_encode(float value);
struct os_linear_color os_color_decode_rgba8(uint32_t rgba,
                                             enum os_pixel_alpha alpha);
uint32_t os_color_encode_rgba8(struct os_linear_color color,
                               enum os_pixel_alpha alpha);
bool os_visual_rect_validate(const struct os_visual_rect *rect);
float os_rounded_rect_coverage(const struct os_visual_rect *rect, float x,
                               float y, float pixel_width);
float os_rounded_rect_border_coverage(const struct os_visual_rect *rect,
                                      float x, float y, float pixel_width);
void os_visual_rotate_point(const struct os_visual_rect *rect, float x, float y,
                            float *out_x, float *out_y);
void os_shadow_cache_init(struct os_shadow_cache *cache);
bool os_shadow_cache_lookup(struct os_shadow_cache *cache,
                            const struct os_shadow_key *key,
                            uint64_t *resource_id);
void os_shadow_cache_insert(struct os_shadow_cache *cache,
                            const struct os_shadow_key *key,
                            uint64_t resource_id, uint64_t *evicted_id);

#endif

/*
 * Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#ifndef OS_COMPOSITOR_BLUR_H
#define OS_COMPOSITOR_BLUR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OS_BLUR_MAX_RADIUS 32u
#define OS_BLUR_MAX_SURFACES 4u
#define OS_BLUR_MAX_TRANSIENT_PIXELS (1280u * 720u)

struct os_effect_rect {
  int32_t x, y, width, height;
};

struct os_blur_budget {
  uint32_t max_radius;
  uint32_t max_surfaces;
  uint64_t max_region_pixels;
  uint64_t max_transient_pixels;
};

struct os_blur_node {
  uint64_t surface_id;
  uint64_t effect_generation;
  struct os_effect_rect region;
  uint32_t radius;
  uint32_t downsample;
  uint32_t tint_rgba8;
};

struct os_effect_graph {
  struct os_blur_budget budget;
  struct os_blur_node nodes[OS_BLUR_MAX_SURFACES];
  size_t node_count;
  uint64_t transient_pixels;
};

struct os_blur_cache {
  struct os_effect_rect captured_region;
  uint64_t surface_id;
  uint64_t effect_generation;
  uint64_t content_generation;
  bool valid;
};

void os_effect_graph_init(struct os_effect_graph *graph,
                          const struct os_blur_budget *budget);
bool os_effect_graph_add_blur(struct os_effect_graph *graph,
                              const struct os_blur_node *node,
                              int32_t output_width, int32_t output_height);
bool os_effect_rect_intersects(const struct os_effect_rect *a,
                               const struct os_effect_rect *b);
bool os_blur_damage_expand(const struct os_effect_rect *region, uint32_t radius,
                           int32_t output_width, int32_t output_height,
                           struct os_effect_rect *out);
void os_blur_cache_invalidate_damage(struct os_blur_cache *cache,
                                     const struct os_effect_rect *damage);
bool os_blur_kernel(uint32_t radius, float *weights, size_t capacity,
                    size_t *count);
bool os_blur_separable_rgba(const float *source, float *temporary,
                            float *destination, uint32_t width, uint32_t height,
                            const float *weights, size_t weight_count);

#endif

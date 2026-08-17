/*
 * Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#include "blur.h"

#include <limits.h>
#include <math.h>
#include <string.h>

static struct os_blur_budget default_budget(void) {
  return (struct os_blur_budget){.max_radius = OS_BLUR_MAX_RADIUS,
                                 .max_surfaces = OS_BLUR_MAX_SURFACES,
                                 .max_region_pixels = 1280u * 720u,
                                 .max_transient_pixels =
                                     OS_BLUR_MAX_TRANSIENT_PIXELS};
}

void os_effect_graph_init(struct os_effect_graph *graph,
                          const struct os_blur_budget *budget) {
  if (graph == NULL)
    return;
  memset(graph, 0, sizeof(*graph));
  graph->budget = budget == NULL ? default_budget() : *budget;
  if (graph->budget.max_surfaces > OS_BLUR_MAX_SURFACES)
    graph->budget.max_surfaces = OS_BLUR_MAX_SURFACES;
}

static bool valid_region(const struct os_effect_rect *region) {
  return region != NULL && region->x >= 0 && region->y >= 0 &&
         region->width > 0 && region->height > 0 &&
         region->x <= INT32_MAX - region->width &&
         region->y <= INT32_MAX - region->height;
}

bool os_effect_graph_add_blur(struct os_effect_graph *graph,
                              const struct os_blur_node *node,
                              int32_t output_width, int32_t output_height) {
  if (graph == NULL || node == NULL || node->surface_id == 0 ||
      !valid_region(&node->region) || node->radius > graph->budget.max_radius ||
      (node->downsample != 2 && node->downsample != 4) || output_width <= 0 ||
      output_height <= 0 ||
      node->region.x + node->region.width > output_width ||
      node->region.y + node->region.height > output_height ||
      graph->node_count >= graph->budget.max_surfaces)
    return false;
  uint64_t region_pixels = (uint64_t)node->region.width * node->region.height;
  uint64_t transient = 2 *
                       ((uint64_t)(node->region.width + node->downsample - 1) /
                        node->downsample) *
                       ((uint64_t)(node->region.height + node->downsample - 1) /
                        node->downsample);
  if (region_pixels > graph->budget.max_region_pixels ||
      transient > graph->budget.max_transient_pixels - graph->transient_pixels)
    return false;
  for (size_t i = 0; i < graph->node_count; ++i)
    if (graph->nodes[i].surface_id == node->surface_id)
      return false;
  graph->nodes[graph->node_count++] = *node;
  graph->transient_pixels += transient;
  return true;
}

bool os_effect_rect_intersects(const struct os_effect_rect *a,
                               const struct os_effect_rect *b) {
  return valid_region(a) && valid_region(b) && a->x < b->x + b->width &&
         b->x < a->x + a->width && a->y < b->y + b->height &&
         b->y < a->y + a->height;
}

bool os_blur_damage_expand(const struct os_effect_rect *region, uint32_t radius,
                           int32_t output_width, int32_t output_height,
                           struct os_effect_rect *out) {
  if (out != NULL)
    memset(out, 0, sizeof(*out));
  if (!valid_region(region) || out == NULL || radius > OS_BLUR_MAX_RADIUS ||
      output_width <= 0 || output_height <= 0)
    return false;
  int64_t left = (int64_t)region->x - radius;
  int64_t top = (int64_t)region->y - radius;
  int64_t right = (int64_t)region->x + region->width + radius;
  int64_t bottom = (int64_t)region->y + region->height + radius;
  left = left < 0 ? 0 : left;
  top = top < 0 ? 0 : top;
  right = right > output_width ? output_width : right;
  bottom = bottom > output_height ? output_height : bottom;
  if (right <= left || bottom <= top)
    return false;
  *out = (struct os_effect_rect){left, top, right - left, bottom - top};
  return true;
}

void os_blur_cache_invalidate_damage(struct os_blur_cache *cache,
                                     const struct os_effect_rect *damage) {
  if (cache != NULL && cache->valid &&
      os_effect_rect_intersects(&cache->captured_region, damage))
    cache->valid = false;
}

bool os_blur_kernel(uint32_t radius, float *weights, size_t capacity,
                    size_t *count) {
  if (count != NULL)
    *count = 0;
  size_t needed = (size_t)radius + 1;
  if (weights == NULL || count == NULL || radius > OS_BLUR_MAX_RADIUS ||
      capacity < needed)
    return false;
  if (radius == 0) {
    weights[0] = 1.0f;
    *count = 1;
    return true;
  }
  float sigma = fmaxf(0.5f, radius / 2.0f), sum = 0.0f;
  for (uint32_t i = 0; i <= radius; ++i) {
    weights[i] = expf(-(float)(i * i) / (2.0f * sigma * sigma));
    sum += i == 0 ? weights[i] : 2.0f * weights[i];
  }
  for (uint32_t i = 0; i <= radius; ++i)
    weights[i] /= sum;
  *count = needed;
  return true;
}

static uint32_t clamp_coordinate(int64_t value, uint32_t limit) {
  return value < 0 ? 0 : (value >= limit ? limit - 1 : (uint32_t)value);
}

bool os_blur_separable_rgba(const float *source, float *temporary,
                            float *destination, uint32_t width, uint32_t height,
                            const float *weights, size_t weight_count) {
  if (source == NULL || temporary == NULL || destination == NULL ||
      weights == NULL || width == 0 || height == 0 || weight_count == 0 ||
      weight_count > OS_BLUR_MAX_RADIUS + 1)
    return false;
  for (uint32_t y = 0; y < height; ++y)
    for (uint32_t x = 0; x < width; ++x)
      for (uint32_t channel = 0; channel < 4; ++channel) {
        float value =
            source[((size_t)y * width + x) * 4 + channel] * weights[0];
        for (size_t tap = 1; tap < weight_count; ++tap) {
          uint32_t left = clamp_coordinate((int64_t)x - tap, width);
          uint32_t right = clamp_coordinate((int64_t)x + tap, width);
          value += (source[((size_t)y * width + left) * 4 + channel] +
                    source[((size_t)y * width + right) * 4 + channel]) *
                   weights[tap];
        }
        temporary[((size_t)y * width + x) * 4 + channel] = value;
      }
  for (uint32_t y = 0; y < height; ++y)
    for (uint32_t x = 0; x < width; ++x)
      for (uint32_t channel = 0; channel < 4; ++channel) {
        float value =
            temporary[((size_t)y * width + x) * 4 + channel] * weights[0];
        for (size_t tap = 1; tap < weight_count; ++tap) {
          uint32_t top = clamp_coordinate((int64_t)y - tap, height);
          uint32_t bottom = clamp_coordinate((int64_t)y + tap, height);
          value += (temporary[((size_t)top * width + x) * 4 + channel] +
                    temporary[((size_t)bottom * width + x) * 4 + channel]) *
                   weights[tap];
        }
        destination[((size_t)y * width + x) * 4 + channel] = value;
      }
  return true;
}

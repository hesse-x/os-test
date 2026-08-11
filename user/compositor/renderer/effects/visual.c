/* Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#include "visual.h"

#include <math.h>
#include <string.h>

static float clamp01(float value) {
  return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

float os_srgb_decode(float value) {
  value = clamp01(value);
  return value <= 0.04045f ? value / 12.92f
                           : powf((value + 0.055f) / 1.055f, 2.4f);
}

float os_srgb_encode(float value) {
  value = clamp01(value);
  return value <= 0.0031308f ? value * 12.92f
                             : 1.055f * powf(value, 1.0f / 2.4f) - 0.055f;
}

struct os_linear_color os_color_decode_rgba8(uint32_t rgba,
                                             enum os_pixel_alpha alpha) {
  struct os_linear_color color = {
      .r = os_srgb_decode(((rgba >> 24) & 0xff) / 255.0f),
      .g = os_srgb_decode(((rgba >> 16) & 0xff) / 255.0f),
      .b = os_srgb_decode(((rgba >> 8) & 0xff) / 255.0f),
      .a = alpha == OS_PIXEL_ALPHA_OPAQUE ? 1.0f : (rgba & 0xff) / 255.0f,
  };
  if (alpha == OS_PIXEL_ALPHA_PREMULTIPLIED && color.a > 0.0f) {
    color.r = clamp01(color.r / color.a);
    color.g = clamp01(color.g / color.a);
    color.b = clamp01(color.b / color.a);
  }
  return color;
}

uint32_t os_color_encode_rgba8(struct os_linear_color color,
                               enum os_pixel_alpha alpha) {
  color.a = alpha == OS_PIXEL_ALPHA_OPAQUE ? 1.0f : clamp01(color.a);
  float multiplier = alpha == OS_PIXEL_ALPHA_PREMULTIPLIED ? color.a : 1.0f;
  uint32_t r = (uint32_t)lroundf(os_srgb_encode(color.r * multiplier) * 255.0f);
  uint32_t g = (uint32_t)lroundf(os_srgb_encode(color.g * multiplier) * 255.0f);
  uint32_t b = (uint32_t)lroundf(os_srgb_encode(color.b * multiplier) * 255.0f);
  uint32_t a = (uint32_t)lroundf(color.a * 255.0f);
  return (r << 24) | (g << 16) | (b << 8) | a;
}

bool os_visual_rect_validate(const struct os_visual_rect *rect) {
  return rect != NULL && isfinite(rect->x) && isfinite(rect->y) &&
         isfinite(rect->width) && isfinite(rect->height) &&
         isfinite(rect->radius) && isfinite(rect->border_width) &&
         isfinite(rect->rotation_radians) && rect->width >= 0.0f &&
         rect->height >= 0.0f && rect->radius >= 0.0f &&
         rect->border_width >= 0.0f;
}

static float rounded_distance(const struct os_visual_rect *rect, float x,
                              float y, float inset) {
  float half_width = fmaxf(0.0f, rect->width * 0.5f - inset);
  float half_height = fmaxf(0.0f, rect->height * 0.5f - inset);
  float radius =
      fminf(fmaxf(0.0f, rect->radius - inset), fminf(half_width, half_height));
  float center_x = rect->x + rect->width * 0.5f;
  float center_y = rect->y + rect->height * 0.5f;
  float cosine = cosf(-rect->rotation_radians);
  float sine = sinf(-rect->rotation_radians);
  float dx = x - center_x, dy = y - center_y;
  float local_x = fabsf(dx * cosine - dy * sine);
  float local_y = fabsf(dx * sine + dy * cosine);
  float qx = local_x - (half_width - radius);
  float qy = local_y - (half_height - radius);
  return hypotf(fmaxf(qx, 0.0f), fmaxf(qy, 0.0f)) + fminf(fmaxf(qx, qy), 0.0f) -
         radius;
}

float os_rounded_rect_coverage(const struct os_visual_rect *rect, float x,
                               float y, float pixel_width) {
  if (!os_visual_rect_validate(rect) || !isfinite(x) || !isfinite(y) ||
      !isfinite(pixel_width) || pixel_width <= 0.0f)
    return 0.0f;
  return clamp01(0.5f - rounded_distance(rect, x, y, 0.0f) / pixel_width);
}

float os_rounded_rect_border_coverage(const struct os_visual_rect *rect,
                                      float x, float y, float pixel_width) {
  float outer = os_rounded_rect_coverage(rect, x, y, pixel_width);
  if (outer == 0.0f || rect->border_width == 0.0f)
    return 0.0f;
  float inner = clamp01(
      0.5f - rounded_distance(rect, x, y, rect->border_width) / pixel_width);
  return clamp01(outer - inner);
}

void os_visual_rotate_point(const struct os_visual_rect *rect, float x, float y,
                            float *out_x, float *out_y) {
  if (!os_visual_rect_validate(rect) || out_x == NULL || out_y == NULL)
    return;
  float cx = rect->x + rect->width * 0.5f;
  float cy = rect->y + rect->height * 0.5f;
  float cosine = cosf(rect->rotation_radians),
        sine = sinf(rect->rotation_radians);
  *out_x = cx + (x - cx) * cosine - (y - cy) * sine;
  *out_y = cy + (x - cx) * sine + (y - cy) * cosine;
}

void os_shadow_cache_init(struct os_shadow_cache *cache) {
  if (cache != NULL)
    memset(cache, 0, sizeof(*cache));
}

bool os_shadow_cache_lookup(struct os_shadow_cache *cache,
                            const struct os_shadow_key *key,
                            uint64_t *resource_id) {
  if (resource_id != NULL)
    *resource_id = 0;
  if (cache == NULL || key == NULL || resource_id == NULL)
    return false;
  for (size_t i = 0; i < OS_SHADOW_CACHE_CAPACITY; ++i) {
    if (cache->entries[i].occupied &&
        memcmp(&cache->entries[i].key, key, sizeof(*key)) == 0) {
      cache->entries[i].last_used = ++cache->clock;
      *resource_id = cache->entries[i].resource_id;
      return true;
    }
  }
  return false;
}

void os_shadow_cache_insert(struct os_shadow_cache *cache,
                            const struct os_shadow_key *key,
                            uint64_t resource_id, uint64_t *evicted_id) {
  if (evicted_id != NULL)
    *evicted_id = 0;
  if (cache == NULL || key == NULL || resource_id == 0)
    return;
  size_t target = 0;
  for (size_t i = 0; i < OS_SHADOW_CACHE_CAPACITY; ++i) {
    if (cache->entries[i].occupied &&
        memcmp(&cache->entries[i].key, key, sizeof(*key)) == 0) {
      target = i;
      goto replace;
    }
    if (!cache->entries[i].occupied) {
      target = i;
      goto replace;
    }
    if (cache->entries[i].last_used < cache->entries[target].last_used)
      target = i;
  }
replace:
  if (cache->entries[target].occupied && evicted_id != NULL)
    *evicted_id = cache->entries[target].resource_id;
  cache->entries[target] = (struct os_shadow_entry){.key = *key,
                                                    .resource_id = resource_id,
                                                    .last_used = ++cache->clock,
                                                    .occupied = true};
}

/* Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#include "dock_layout.h"

#include <string.h>

#define DOCK_MIN_HEIGHT 48
#define DOCK_MAX_HEIGHT 72

static int32_t clamp_int(int32_t value, int32_t low, int32_t high) {
  return value < low ? low : (value > high ? high : value);
}

static void update_dimensions(struct os_dock_layout *layout) {
  int32_t target_height = clamp_int(layout->output_height * 3 / 40,
                                    DOCK_MIN_HEIGHT, DOCK_MAX_HEIGHT);
  layout->icon_size = target_height * 2 / 3;
  layout->padding = clamp_int(target_height * 2 / 9, 10, 16);
  int32_t slot = layout->icon_size + 2 * layout->padding;
  size_t count = layout->target_count == 0 ? 1 : layout->target_count;
  layout->width = slot * (int32_t)count;
  layout->height = (slot * 10 + 8) / 9;
}

bool os_dock_layout_init(struct os_dock_layout *layout, int32_t output_width,
                         int32_t output_height, int32_t scale) {
  if (layout == NULL || output_width <= 0 || output_height <= 0 || scale <= 0)
    return false;
  memset(layout, 0, sizeof(*layout));
  layout->output_width = output_width;
  layout->output_height = output_height;
  layout->scale = scale;
  layout->generation = 1;
  update_dimensions(layout);
  return true;
}

bool os_dock_layout_set_targets(struct os_dock_layout *layout,
                                const uint64_t *ids, size_t count) {
  if (layout == NULL || count > OS_DOCK_MAX_TARGETS ||
      (count != 0 && ids == NULL))
    return false;
  for (size_t i = 0; i < count; ++i) {
    if (ids[i] == 0)
      return false;
    for (size_t j = 0; j < i; ++j)
      if (ids[i] == ids[j])
        return false;
  }
  if (count != 0)
    memcpy(layout->target_ids, ids, count * sizeof(*ids));
  layout->target_count = count;
  update_dimensions(layout);
  ++layout->generation;
  return true;
}

bool os_dock_layout_target(const struct os_dock_layout *layout,
                           uint64_t target_id, struct os_dock_target *out) {
  if (out != NULL)
    memset(out, 0, sizeof(*out));
  if (layout == NULL || out == NULL || target_id == 0)
    return false;
  size_t count = layout->target_count == 0 ? 1 : layout->target_count;
  size_t index = 0;
  if (layout->target_count != 0) {
    for (; index < count && layout->target_ids[index] != target_id; ++index) {
    }
    if (index == count)
      return false;
  } else if (target_id != 1) {
    return false;
  }
  int32_t slot = layout->width / (int32_t)count;
  out->dock_target_id = target_id;
  out->geometry.x = (layout->output_width - layout->width) / 2.0 +
                    index * slot + layout->padding;
  out->geometry.y = layout->output_height - layout->height + layout->padding;
  out->geometry.width = layout->icon_size;
  out->geometry.height = layout->icon_size;
  out->geometry.scale = layout->scale;
  out->generation = layout->generation;
  return true;
}

bool os_dock_layout_hit_test(const struct os_dock_layout *layout, double x,
                             double y, uint64_t *target_id) {
  if (target_id != NULL)
    *target_id = 0;
  if (layout == NULL || target_id == NULL || x < 0 || y < 0 ||
      x >= layout->width || y >= layout->height)
    return false;
  size_t count = layout->target_count == 0 ? 1 : layout->target_count;
  size_t index = (size_t)(x / (layout->width / (double)count));
  if (index >= count)
    return false;
  *target_id = layout->target_count == 0 ? 1 : layout->target_ids[index];
  return true;
}

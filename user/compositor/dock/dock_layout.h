/* Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#ifndef OS_COMPOSITOR_DOCK_LAYOUT_H
#define OS_COMPOSITOR_DOCK_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OS_DOCK_MAX_TARGETS 32u
#define OS_DOCK_HOVER_SCALE 1.12

struct os_logical_rect {
  double x;
  double y;
  double width;
  double height;
  int32_t scale;
};

struct os_dock_target {
  uint64_t dock_target_id;
  struct os_logical_rect geometry;
  uint64_t generation;
};

struct os_dock_icon_geometry {
  double x;
  double y;
  double size;
};

struct os_dock_layout {
  int32_t output_width;
  int32_t output_height;
  int32_t scale;
  int32_t icon_size;
  int32_t padding;
  int32_t width;
  int32_t height;
  uint64_t generation;
  size_t target_count;
  uint64_t target_ids[OS_DOCK_MAX_TARGETS];
};

bool os_dock_layout_init(struct os_dock_layout *layout, int32_t output_width,
                         int32_t output_height, int32_t scale);
bool os_dock_layout_set_targets(struct os_dock_layout *layout,
                                const uint64_t *ids, size_t count);
bool os_dock_layout_target(const struct os_dock_layout *layout,
                           uint64_t target_id, struct os_dock_target *out);
bool os_dock_layout_hit_test(const struct os_dock_layout *layout, double x,
                             double y, uint64_t *target_id);
void os_dock_magnify_icons(double width, double height, double icon_size,
                           double padding, size_t count, double pointer_x,
                           bool pointer_active,
                           struct os_dock_icon_geometry *icons);
void os_dock_layout_icons(const struct os_dock_layout *layout, double pointer_x,
                          bool pointer_active,
                          struct os_dock_icon_geometry *icons);

#endif

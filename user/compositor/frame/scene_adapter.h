/* Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#ifndef OS_COMPOSITOR_SCENE_ADAPTER_H
#define OS_COMPOSITOR_SCENE_ADAPTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OS_SCENE_ADAPTER_VERSION 1u

struct wlr_scene_output;

struct os_scene_draw_item {
  const void *scene_buffer;
  const void *buffer;
  int32_t dst_x, dst_y, dst_width, dst_height;
  float src_x, src_y, src_width, src_height;
  float opacity;
  uint32_t transform;
};

typedef bool (*os_scene_snapshot_fn)(void *data,
                                     struct wlr_scene_output *output,
                                     const struct os_scene_draw_item **items,
                                     size_t *item_count);

struct os_scene_adapter {
  uint32_t size;
  uint32_t version;
  os_scene_snapshot_fn snapshot;
  void *data;
};

bool os_scene_adapter_validate(const struct os_scene_adapter *adapter);

#endif

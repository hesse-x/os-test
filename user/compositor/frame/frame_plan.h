/*
 * Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#ifndef OS_COMPOSITOR_FRAME_PLAN_H
#define OS_COMPOSITOR_FRAME_PLAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OS_FRAME_PLAN_VERSION 1u
#define OS_FRAME_PLAN_MAX_ITEMS 4096u

enum os_frame_item_type {
  OS_FRAME_ITEM_SCENE,
  OS_FRAME_ITEM_MESH,
};

struct os_frame_item {
  enum os_frame_item_type type;
  uint64_t resource_id;
  const void *payload;
};

struct os_frame_plan {
  uint32_t size;
  uint32_t version;
  const struct os_frame_item *items;
  size_t item_count;
  bool full_damage;
};

typedef bool (*os_legacy_scene_commit_fn)(void *scene_output);

bool os_frame_plan_validate(const struct os_frame_plan *plan);
bool os_frame_submit_legacy(void *scene_output,
                            os_legacy_scene_commit_fn commit);

#endif

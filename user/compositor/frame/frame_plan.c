/* Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#include "frame_plan.h"

bool os_frame_plan_validate(const struct os_frame_plan *plan) {
  if (plan == NULL || plan->size != sizeof(*plan) ||
      plan->version != OS_FRAME_PLAN_VERSION ||
      plan->item_count > OS_FRAME_PLAN_MAX_ITEMS ||
      (plan->item_count != 0 && plan->items == NULL))
    return false;
  for (size_t i = 0; i < plan->item_count; ++i) {
    if (plan->items[i].type != OS_FRAME_ITEM_SCENE &&
        plan->items[i].type != OS_FRAME_ITEM_MESH)
      return false;
  }
  return true;
}

bool os_frame_submit_legacy(void *scene_output,
                            os_legacy_scene_commit_fn commit) {
  return scene_output != NULL && commit != NULL && commit(scene_output);
}

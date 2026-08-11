/* Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#include <stdbool.h>
#include <wlr/types/wlr_scene.h>

#include "../frame/frame_plan.h"
#include "frame.h"

static bool commit_scene(void *scene_output) {
  return wlr_scene_output_commit(scene_output, NULL);
}

bool os_core_commit_scene_frame(struct wlr_scene_output *scene_output) {
  return os_frame_submit_legacy(scene_output, commit_scene);
}

/* Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#include <stdbool.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_damage_ring.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>

#include "../frame/frame_plan.h"
#include "frame.h"

static struct wlr_output_cursor *
suspend_software_cursor(struct wlr_output *output) {
  struct wlr_output_cursor *cursor;
  wl_list_for_each(cursor, &output->cursors, link) {
    if (cursor != output->hardware_cursor && cursor->enabled) {
      cursor->enabled = false;
      return cursor;
    }
  }
  return NULL;
}

bool os_core_commit_scene_frame(struct wlr_scene_output *scene_output,
                                struct wlr_renderer *renderer,
                                os_frame_overlay_fn overlay, void *data) {
  if (scene_output == NULL || renderer == NULL)
    return false;
  if (overlay != NULL) {
    wlr_damage_ring_add_whole(&scene_output->damage_ring);
    pixman_region32_union_rect(&scene_output->pending_commit_damage,
                               &scene_output->pending_commit_damage, 0, 0,
                               scene_output->output->width,
                               scene_output->output->height);
  }
  struct wlr_output_state state;
  wlr_output_state_init(&state);
  // This compositor owns one wlr_cursor, hence at most one output cursor.
  struct wlr_output_cursor *software_cursor =
      overlay != NULL ? suspend_software_cursor(scene_output->output) : NULL;
  bool success = wlr_scene_output_build_state(scene_output, &state, NULL);
  if (software_cursor != NULL)
    software_cursor->enabled = true;
  if (success && overlay != NULL && state.buffer != NULL) {
    struct wlr_render_pass *pass =
        wlr_renderer_begin_buffer_pass(renderer, state.buffer, NULL);
    if (pass != NULL && overlay(pass, data)) {
      // Custom overlays must stay below the pointer, matching hardware cursor
      // plane semantics when wlroots falls back to software cursors.
      wlr_output_add_software_cursors_to_render_pass(scene_output->output, pass,
                                                     NULL);
      success = wlr_render_pass_submit(pass);
    } else {
      success = false;
    }
  }
  if (success)
    success = wlr_output_commit_state(scene_output->output, &state);
  wlr_output_state_finish(&state);
  return success;
}

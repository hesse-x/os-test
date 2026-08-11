/* Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#ifndef OS_COMPOSITOR_CORE_FRAME_H
#define OS_COMPOSITOR_CORE_FRAME_H

#include <stdbool.h>

struct wlr_scene_output;
struct wlr_renderer;
struct wlr_render_pass;

typedef bool (*os_frame_overlay_fn)(struct wlr_render_pass *pass, void *data);

bool os_core_commit_scene_frame(struct wlr_scene_output *scene_output,
                                struct wlr_renderer *renderer,
                                os_frame_overlay_fn overlay, void *data);

#endif

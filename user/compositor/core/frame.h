/* Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#ifndef OS_COMPOSITOR_CORE_FRAME_H
#define OS_COMPOSITOR_CORE_FRAME_H

#include <stdbool.h>

struct wlr_scene_output;

bool os_core_commit_scene_frame(struct wlr_scene_output *scene_output);

#endif

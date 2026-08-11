/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef TINYWL_GENIE_H
#define TINYWL_GENIE_H

#include <stdbool.h>
#include <stddef.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>

struct tinywl_genie_animation;

struct tinywl_genie_options {
  struct wl_event_loop *event_loop;
  struct wlr_scene_tree *animation_parent;
  struct wlr_scene_node *source;
  int source_x, source_y;
  struct wlr_scene_rect **rects;
  size_t rect_count;
  struct wlr_box window;
  double target_x, target_y;
  double target_width, target_height;
  bool minimizing;
  int duration_ms;
  void (*finished)(void *data, bool minimizing);
  void *data;
};

struct tinywl_genie_animation *
tinywl_genie_start(const struct tinywl_genie_options *options);
void tinywl_genie_cancel(struct tinywl_genie_animation *animation);
void tinywl_genie_reverse(struct tinywl_genie_animation *animation);
double tinywl_genie_progress(const struct tinywl_genie_animation *animation);
bool tinywl_genie_validate_model(void);

#endif

/*
 * Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#ifndef OS_COMPOSITOR_GENIE_RUNTIME_H
#define OS_COMPOSITOR_GENIE_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>

struct wlr_renderer;
struct wlr_render_pass;
struct wlr_output;
struct tinywl_genie_animation;

struct tinywl_genie_options {
  struct wl_event_loop *event_loop;
  struct wlr_renderer *renderer;
  struct wlr_output *output;
  struct wlr_scene_node *source;
  int source_x, source_y;
  struct wlr_scene_rect **rects;
  size_t rect_count;
  struct wlr_box snapshot_window;
  struct wlr_box window;
  double target_x, target_y, target_width, target_height;
  uint64_t target_id, target_generation;
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
bool tinywl_genie_render(struct tinywl_genie_animation *animation,
                         struct wlr_render_pass *pass);
bool tinywl_genie_targets(const struct tinywl_genie_animation *animation,
                          uint64_t target_id, uint64_t generation);
bool tinywl_genie_uses_output(const struct tinywl_genie_animation *animation,
                              const struct wlr_output *output);

#endif

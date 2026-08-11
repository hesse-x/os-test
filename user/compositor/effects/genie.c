/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#include "genie.h"

#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <wlr/util/log.h>

#define GENIE_STRIP_HEIGHT 18
#define GENIE_MAX_STRIPS 48

enum genie_piece_type {
  GENIE_PIECE_BUFFER,
  GENIE_PIECE_RECT,
};

struct genie_piece {
  struct wl_list link;
  enum genie_piece_type type;
  union {
    struct wlr_scene_buffer *buffer;
    struct wlr_scene_rect *rect;
  };
  double x, y, width, height;
};

struct tinywl_genie_animation {
  struct tinywl_genie_options options;
  struct wlr_scene_tree *tree;
  struct wl_event_source *timer;
  struct wl_list pieces;
  struct timespec last_frame;
  double progress;
  int direction;
};

struct genie_point {
  double x, y;
};

static double clamp01(double value) { return fmax(0.0, fmin(1.0, value)); }

static double smoothstep(double value) {
  value = clamp01(value);
  return value * value * (3.0 - 2.0 * value);
}

static double mix(double from, double to, double amount) {
  return from + (to - from) * amount;
}

static double cubic_bezier(double from, double control1, double control2,
                           double to, double amount) {
  double inverse = 1.0 - amount;
  return inverse * inverse * inverse * from +
         3.0 * inverse * inverse * amount * control1 +
         3.0 * inverse * amount * amount * control2 +
         amount * amount * amount * to;
}

/* Pure progress-based geometry keeps an in-flight animation reversible. */
static struct genie_point warp_point(const struct tinywl_genie_options *options,
                                     double nx, double ny, double progress) {
  double target_width = options->target_width > 0 ? options->target_width : 56;
  double target_height =
      options->target_height > 0 ? options->target_height : 4;
  double row_delay = (1.0 - ny) * 0.58;
  double row_progress = smoothstep((progress - row_delay) / (1.0 - row_delay));
  double transition = sin(M_PI * progress);
  double source_center = options->window.x + options->window.width * 0.5;
  double center_delta = options->target_x - source_center;
  double center = cubic_bezier(
      source_center, source_center + center_delta * 0.10,
      options->target_x - center_delta * 0.16, options->target_x, row_progress);
  double width =
      cubic_bezier(options->window.width, options->window.width * 1.01,
                   target_width * 1.85, target_width, row_progress);
  double fold = sin(M_PI * ny) * transition;
  width *= 1.0 - 0.045 * fold * sin(M_PI * clamp01(ny + progress * 0.55));
  double source_y = options->window.y + options->window.height * ny;
  double target_y = options->target_y + (ny - 0.5) * target_height;
  return (struct genie_point){
      .x = center + (nx - 0.5) * width,
      .y = mix(source_y, target_y, row_progress),
  };
}

bool tinywl_genie_validate_model(void) {
  const struct tinywl_genie_options options = {
      .window = {.x = 80, .y = 60, .width = 640, .height = 480},
      .target_x = 620,
      .target_y = 700,
      .target_width = 56,
      .target_height = 4,
  };
  for (int step = 0; step <= 20; step++) {
    double progress = step / 20.0;
    for (int row = 0; row < 24; row++) {
      double ny = row / 23.0;
      struct genie_point left = warp_point(&options, 0, ny, progress);
      struct genie_point right = warp_point(&options, 1, ny, progress);
      if (!isfinite(left.x) || !isfinite(left.y) || !isfinite(right.x) ||
          !isfinite(right.y) || right.x <= left.x) {
        return false;
      }
    }
  }
  return true;
}

static void destroy_animation(struct tinywl_genie_animation *animation) {
  if (animation->timer != NULL) {
    wl_event_source_remove(animation->timer);
  }
  struct genie_piece *piece, *tmp;
  wl_list_for_each_safe(piece, tmp, &animation->pieces, link) {
    wl_list_remove(&piece->link);
    free(piece);
  }
  if (animation->tree != NULL) {
    wlr_scene_node_destroy(&animation->tree->node);
  }
  free(animation);
}

void tinywl_genie_cancel(struct tinywl_genie_animation *animation) {
  if (animation != NULL) {
    destroy_animation(animation);
  }
}

void tinywl_genie_reverse(struct tinywl_genie_animation *animation) {
  if (animation == NULL) {
    return;
  }
  animation->direction = -animation->direction;
  animation->options.minimizing = animation->direction > 0;
  clock_gettime(CLOCK_MONOTONIC, &animation->last_frame);
}

double tinywl_genie_progress(const struct tinywl_genie_animation *animation) {
  return animation == NULL ? 0.0 : animation->progress;
}

static void update_piece(struct tinywl_genie_animation *animation,
                         struct genie_piece *piece, double progress) {
  const struct wlr_box *window = &animation->options.window;
  double nx0 = (piece->x - window->x) / window->width;
  double nx1 = (piece->x + piece->width - window->x) / window->width;
  double ny0 = (piece->y - window->y) / window->height;
  double ny1 = (piece->y + piece->height - window->y) / window->height;
  double middle_y = (ny0 + ny1) * 0.5;
  struct genie_point left =
      warp_point(&animation->options, nx0, middle_y, progress);
  struct genie_point right =
      warp_point(&animation->options, nx1, middle_y, progress);
  struct genie_point top = warp_point(&animation->options, 0.5, ny0, progress);
  struct genie_point bottom =
      warp_point(&animation->options, 0.5, ny1, progress);
  int x = (int)floor(left.x);
  int y = (int)floor(fmin(top.y, bottom.y));
  int width = (int)ceil(right.x) - x;
  int height = (int)ceil(fmax(top.y, bottom.y)) - y;
  width = width > 0 ? width : 1;
  height = height > 0 ? height : 1;
  if (piece->type == GENIE_PIECE_BUFFER) {
    wlr_scene_node_set_position(&piece->buffer->node, x, y);
    wlr_scene_buffer_set_dest_size(piece->buffer, width, height);
  } else {
    wlr_scene_node_set_position(&piece->rect->node, x, y);
    wlr_scene_rect_set_size(piece->rect, width, height);
  }
}

static void update_animation(struct tinywl_genie_animation *animation,
                             double progress) {
  struct genie_piece *piece;
  wl_list_for_each(piece, &animation->pieces, link) {
    update_piece(animation, piece, progress);
  }
}

static int animation_timer(void *data) {
  struct tinywl_genie_animation *animation = data;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  double elapsed_ms = (now.tv_sec - animation->last_frame.tv_sec) * 1000.0 +
                      (now.tv_nsec - animation->last_frame.tv_nsec) / 1000000.0;
  animation->last_frame = now;
  animation->progress =
      clamp01(animation->progress + animation->direction * elapsed_ms /
                                        animation->options.duration_ms);
  update_animation(animation, animation->progress);
  bool complete = animation->direction > 0 ? animation->progress >= 1.0
                                           : animation->progress <= 0.0;
  if (complete) {
    void (*finished)(void *, bool) = animation->options.finished;
    void *finished_data = animation->options.data;
    bool minimizing = animation->direction > 0;
    animation->timer = NULL;
    destroy_animation(animation);
    if (finished != NULL) {
      finished(finished_data, minimizing);
    }
    return 0;
  }
  wl_event_source_timer_update(animation->timer, 16);
  return 0;
}

static int strip_count(double height) {
  int count = (int)ceil(height / GENIE_STRIP_HEIGHT);
  if (count < 1) {
    count = 1;
  }
  return count > GENIE_MAX_STRIPS ? GENIE_MAX_STRIPS : count;
}

static void add_buffer_strips(struct wlr_scene_buffer *source, int sx, int sy,
                              void *data) {
  struct tinywl_genie_animation *animation = data;
  if (source->buffer == NULL ||
      source->transform != WL_OUTPUT_TRANSFORM_NORMAL) {
    return;
  }
  struct wlr_fbox source_box = source->src_box;
  if (source_box.width <= 0 || source_box.height <= 0) {
    source_box = (struct wlr_fbox){
        .width = source->buffer->width,
        .height = source->buffer->height,
    };
  }
  double width = source->dst_width > 0 ? source->dst_width : source_box.width;
  double height =
      source->dst_height > 0 ? source->dst_height : source_box.height;
  int count = strip_count(height);
  for (int row = 0; row < count; row++) {
    double top = height * row / count;
    double bottom = height * (row + 1) / count;
    struct genie_piece *piece = calloc(1, sizeof(*piece));
    if (piece == NULL) {
      return;
    }
    piece->type = GENIE_PIECE_BUFFER;
    piece->x = animation->options.source_x + sx;
    piece->y = animation->options.source_y + sy + top;
    piece->width = width;
    piece->height = bottom - top;
    piece->buffer = wlr_scene_buffer_create(animation->tree, source->buffer);
    if (piece->buffer == NULL) {
      free(piece);
      return;
    }
    struct wlr_fbox strip_box = {
        .x = source_box.x,
        .y = source_box.y + source_box.height * row / count,
        .width = source_box.width,
        .height = source_box.height / count,
    };
    wlr_scene_buffer_set_source_box(piece->buffer, &strip_box);
    wlr_scene_buffer_set_opacity(piece->buffer, source->opacity);
    wlr_scene_buffer_set_filter_mode(piece->buffer, WLR_SCALE_FILTER_BILINEAR);
    wl_list_insert(animation->pieces.prev, &piece->link);
  }
}

static void add_rect_strips(struct tinywl_genie_animation *animation,
                            struct wlr_scene_rect *source) {
  if (source == NULL || source->width <= 0 || source->height <= 0) {
    return;
  }
  int x, y;
  wlr_scene_node_coords(&source->node, &x, &y);
  int count = strip_count(source->height);
  for (int row = 0; row < count; row++) {
    double top = (double)source->height * row / count;
    double bottom = (double)source->height * (row + 1) / count;
    struct genie_piece *piece = calloc(1, sizeof(*piece));
    if (piece == NULL) {
      return;
    }
    piece->type = GENIE_PIECE_RECT;
    piece->x = x;
    piece->y = y + top;
    piece->width = source->width;
    piece->height = bottom - top;
    piece->rect =
        wlr_scene_rect_create(animation->tree, source->width,
                              (int)ceil(piece->height), source->color);
    if (piece->rect == NULL) {
      free(piece);
      return;
    }
    wl_list_insert(animation->pieces.prev, &piece->link);
  }
}

struct tinywl_genie_animation *
tinywl_genie_start(const struct tinywl_genie_options *options) {
  if (options == NULL || options->event_loop == NULL ||
      options->animation_parent == NULL || options->source == NULL ||
      options->window.width <= 0 || options->window.height <= 0 ||
      options->duration_ms <= 0 || !isfinite(options->target_x) ||
      !isfinite(options->target_y) || !tinywl_genie_validate_model()) {
    return NULL;
  }
  struct tinywl_genie_animation *animation = calloc(1, sizeof(*animation));
  if (animation == NULL) {
    return NULL;
  }
  animation->options = *options;
  animation->progress = options->minimizing ? 0.0 : 1.0;
  animation->direction = options->minimizing ? 1 : -1;
  wl_list_init(&animation->pieces);
  animation->tree = wlr_scene_tree_create(options->animation_parent);
  if (animation->tree == NULL) {
    destroy_animation(animation);
    return NULL;
  }
  wlr_scene_node_for_each_buffer(options->source, add_buffer_strips, animation);
  for (size_t i = 0; i < options->rect_count; i++) {
    add_rect_strips(animation, options->rects[i]);
  }
  if (wl_list_empty(&animation->pieces)) {
    destroy_animation(animation);
    return NULL;
  }
  update_animation(animation, animation->progress);
  wlr_scene_node_raise_to_top(&animation->tree->node);
  clock_gettime(CLOCK_MONOTONIC, &animation->last_frame);
  animation->timer =
      wl_event_loop_add_timer(options->event_loop, animation_timer, animation);
  if (animation->timer == NULL) {
    destroy_animation(animation);
    return NULL;
  }
  wl_event_source_timer_update(animation->timer, 16);
  wlr_log(WLR_INFO, "genie.start direction=%s target=%.0f,%.0f duration_ms=%d",
          options->minimizing ? "minimize" : "restore", options->target_x,
          options->target_y, options->duration_ms);
  return animation;
}

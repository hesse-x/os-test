/*
 * Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#include "genie_runtime.h"

#include <drm_fourcc.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_output.h>

#include "genie_mesh.h"
#include "renderer/vulkan/os_vk_renderer.h"

#define GENIE_MAX_COLUMNS 32u
#define GENIE_MAX_ROWS 48u
#define GENIE_CELL_WIDTH 28u
#define GENIE_CELL_HEIGHT 18u
#define GENIE_MAX_FRAME_NS 32000000ull
#define GENIE_VERTEX_COUNT ((GENIE_MAX_COLUMNS + 1) * (GENIE_MAX_ROWS + 1))
#define GENIE_INDEX_COUNT (GENIE_MAX_COLUMNS * GENIE_MAX_ROWS * 6)

struct tinywl_genie_animation {
  struct tinywl_genie_options options;
  struct os_genie_animation model;
  struct os_genie_target target;
  struct os_mesh mesh;
  struct os_mesh_vertex vertices[GENIE_VERTEX_COUNT];
  uint32_t indices[GENIE_INDEX_COUNT];
  uint32_t columns, rows;
  struct wlr_texture *texture;
  struct wl_event_source *timer;
  struct timespec last_tick;
};

struct snapshot_context {
  struct tinywl_genie_animation *animation;
  uint8_t *pixels;
  bool valid;
  size_t buffer_count;
};

static float clamp01(float value) {
  return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

static bool unpack_pixel(uint32_t format, const uint8_t pixel[4],
                         uint8_t rgba[4]) {
  switch (format) {
  case DRM_FORMAT_ABGR8888:
    memcpy(rgba, pixel, 4);
    return true;
  case DRM_FORMAT_XBGR8888:
    memcpy(rgba, pixel, 3);
    rgba[3] = 255;
    return true;
  case DRM_FORMAT_ARGB8888:
    rgba[0] = pixel[2];
    rgba[1] = pixel[1];
    rgba[2] = pixel[0];
    rgba[3] = pixel[3];
    return true;
  case DRM_FORMAT_XRGB8888:
    rgba[0] = pixel[2];
    rgba[1] = pixel[1];
    rgba[2] = pixel[0];
    rgba[3] = 255;
    return true;
  default:
    return false;
  }
}

static void blend_rgba(uint8_t destination[4], const uint8_t source[4],
                       float opacity) {
  uint32_t alpha = (uint32_t)lroundf(source[3] * clamp01(opacity));
  uint32_t inverse = 255 - alpha;
  for (size_t channel = 0; channel < 3; ++channel) {
    uint32_t value = (uint32_t)lroundf(source[channel] * clamp01(opacity));
    destination[channel] = (uint8_t)fminf(
        255, value + (destination[channel] * inverse + 127) / 255);
  }
  destination[3] =
      (uint8_t)fminf(255, alpha + (destination[3] * inverse + 127) / 255);
}

static void snapshot_buffer(struct wlr_scene_buffer *source, int sx, int sy,
                            void *data) {
  struct snapshot_context *context = data;
  if (!context->valid || source->buffer == NULL)
    return;
  if (source->transform != WL_OUTPUT_TRANSFORM_NORMAL) {
    context->valid = false;
    return;
  }
  struct tinywl_genie_animation *animation = context->animation;
  struct wlr_texture *texture =
      wlr_texture_from_buffer(animation->options.renderer, source->buffer);
  if (texture == NULL || texture->width > SIZE_MAX / 4 / texture->height) {
    wlr_texture_destroy(texture);
    context->valid = false;
    return;
  }
  uint32_t format = wlr_texture_preferred_read_format(texture);
  size_t stride = (size_t)texture->width * 4;
  uint8_t *source_pixels = malloc(stride * texture->height);
  if (source_pixels == NULL ||
      !wlr_texture_read_pixels(texture,
                               &(struct wlr_texture_read_pixels_options){
                                   .data = source_pixels,
                                   .format = format,
                                   .stride = stride,
                               })) {
    free(source_pixels);
    wlr_texture_destroy(texture);
    context->valid = false;
    return;
  }
  uint8_t probe[4];
  bool supported = unpack_pixel(format, source_pixels, probe);
  uint32_t texture_width = texture->width;
  uint32_t texture_height = texture->height;
  wlr_texture_destroy(texture);
  if (!supported) {
    free(source_pixels);
    context->valid = false;
    return;
  }

  struct wlr_fbox source_box = source->src_box;
  if (source_box.width <= 0 || source_box.height <= 0) {
    source_box = (struct wlr_fbox){.width = source->buffer->width,
                                   .height = source->buffer->height};
  }
  int display_width =
      source->dst_width > 0 ? source->dst_width : (int)lround(source_box.width);
  int display_height = source->dst_height > 0 ? source->dst_height
                                              : (int)lround(source_box.height);
  const struct wlr_box *window = &animation->options.snapshot_window;
  int destination_x = animation->options.source_x + sx - window->x;
  int destination_y = animation->options.source_y + sy - window->y;
  for (int y = 0; y < display_height; ++y) {
    int dy = destination_y + y;
    if (dy < 0 || dy >= window->height)
      continue;
    uint32_t sample_y = (uint32_t)fmin(
        texture_height - 1,
        floor(source_box.y + (y + 0.5) * source_box.height / display_height));
    for (int x = 0; x < display_width; ++x) {
      int dx = destination_x + x;
      if (dx < 0 || dx >= window->width)
        continue;
      uint32_t sample_x = (uint32_t)fmin(
          texture_width - 1,
          floor(source_box.x + (x + 0.5) * source_box.width / display_width));
      uint8_t rgba[4];
      unpack_pixel(format,
                   &source_pixels[(size_t)sample_y * stride + sample_x * 4],
                   rgba);
      blend_rgba(&context->pixels[((size_t)dy * window->width + dx) * 4], rgba,
                 source->opacity);
    }
  }
  ++context->buffer_count;
  free(source_pixels);
}

static void snapshot_rect(struct snapshot_context *context,
                          struct wlr_scene_rect *rect) {
  if (rect == NULL || rect->width <= 0 || rect->height <= 0)
    return;
  int global_x, global_y;
  wlr_scene_node_coords(&rect->node, &global_x, &global_y);
  const struct wlr_box *window = &context->animation->options.snapshot_window;
  uint8_t rgba[4];
  for (size_t channel = 0; channel < 4; ++channel)
    rgba[channel] = (uint8_t)lroundf(clamp01(rect->color[channel]) * 255);
  for (int y = 0; y < rect->height; ++y) {
    int dy = global_y - window->y + y;
    if (dy < 0 || dy >= window->height)
      continue;
    for (int x = 0; x < rect->width; ++x) {
      int dx = global_x - window->x + x;
      if (dx < 0 || dx >= window->width)
        continue;
      blend_rgba(&context->pixels[((size_t)dy * window->width + dx) * 4], rgba,
                 1.0f);
    }
  }
}

static bool create_window_snapshot(struct tinywl_genie_animation *animation) {
  const struct wlr_box *window = &animation->options.snapshot_window;
  if (window->width <= 0 || window->height <= 0 ||
      (size_t)window->width > SIZE_MAX / 4 / window->height)
    return false;
  struct snapshot_context context = {
      .animation = animation,
      .pixels = calloc((size_t)window->width * window->height, 4),
      .valid = true,
  };
  if (context.pixels == NULL)
    return false;
  // SSD buttons are buffers, so paint the flat frame first and composite all
  // scene buffers afterward in their normal traversal order.
  for (size_t i = 0; context.valid && i < animation->options.rect_count; ++i)
    snapshot_rect(&context, animation->options.rects[i]);
  wlr_scene_node_for_each_buffer(animation->options.source, snapshot_buffer,
                                 &context);
  if (!context.valid || context.buffer_count == 0) {
    free(context.pixels);
    return false;
  }
  animation->texture = wlr_texture_from_pixels(
      animation->options.renderer, DRM_FORMAT_ABGR8888, window->width * 4,
      window->width, window->height, context.pixels);
  free(context.pixels);
  return animation->texture != NULL;
}

static void release_texture(void *handle) {
  struct tinywl_genie_animation *animation = handle;
  wlr_texture_destroy(animation->texture);
  animation->texture = NULL;
}

static void destroy_animation(struct tinywl_genie_animation *animation) {
  if (animation->timer != NULL)
    wl_event_source_remove(animation->timer);
  os_genie_destroy(&animation->model);
  free(animation);
}

static int animation_tick(void *data) {
  struct tinywl_genie_animation *animation = data;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  int64_t elapsed_signed =
      (int64_t)(now.tv_sec - animation->last_tick.tv_sec) * 1000000000ll +
      (int64_t)now.tv_nsec - animation->last_tick.tv_nsec;
  uint64_t elapsed = elapsed_signed > 0 ? (uint64_t)elapsed_signed : 0;
  if (elapsed > GENIE_MAX_FRAME_NS)
    elapsed = GENIE_MAX_FRAME_NS;
  animation->last_tick = now;
  if (!os_genie_step(&animation->model, elapsed, &animation->target,
                     &animation->mesh, animation->columns, animation->rows)) {
    tinywl_genie_cancel(animation);
    return 0;
  }
  if (animation->options.output != NULL)
    wlr_output_schedule_frame(animation->options.output);
  if (animation->model.status == OS_GENIE_FINISHED) {
    void (*finished)(void *, bool) = animation->options.finished;
    void *finished_data = animation->options.data;
    bool minimizing = animation->model.direction == OS_GENIE_MINIMIZE;
    animation->timer = NULL;
    destroy_animation(animation);
    if (finished != NULL)
      finished(finished_data, minimizing);
    return 0;
  }
  wl_event_source_timer_update(animation->timer, 16);
  return 0;
}

struct tinywl_genie_animation *
tinywl_genie_start(const struct tinywl_genie_options *options) {
  if (options == NULL || options->event_loop == NULL ||
      options->renderer == NULL || options->output == NULL ||
      options->source == NULL || options->window.width <= 0 ||
      options->window.height <= 0 || options->duration_ms <= 0)
    return NULL;
  struct tinywl_genie_animation *animation = calloc(1, sizeof(*animation));
  if (animation == NULL)
    return NULL;
  animation->options = *options;
  if (!create_window_snapshot(animation)) {
    free(animation);
    return NULL;
  }
  animation->options.rects = NULL;
  animation->options.rect_count = 0;
  animation->target = (struct os_genie_target){
      .id = options->target_id != 0 ? options->target_id : 1,
      .generation =
          options->target_generation != 0 ? options->target_generation : 1,
      .x = options->target_x - options->target_width * 0.5,
      .y = options->target_y - options->target_height * 0.5,
      .width = options->target_width,
      .height = options->target_height,
  };
  struct os_genie_snapshot snapshot = {
      .handle = animation,
      .width = options->snapshot_window.width,
      .height = options->snapshot_window.height,
      .release = release_texture,
  };
  animation->mesh = (struct os_mesh){
      .vertices = animation->vertices,
      .indices = animation->indices,
      .vertex_capacity = GENIE_VERTEX_COUNT,
      .index_capacity = GENIE_INDEX_COUNT,
  };
  animation->columns =
      ((uint32_t)options->window.width + GENIE_CELL_WIDTH - 1) /
      GENIE_CELL_WIDTH;
  animation->rows = ((uint32_t)options->window.height + GENIE_CELL_HEIGHT - 1) /
                    GENIE_CELL_HEIGHT;
  if (animation->columns < 1)
    animation->columns = 1;
  if (animation->columns > GENIE_MAX_COLUMNS)
    animation->columns = GENIE_MAX_COLUMNS;
  if (animation->rows < 1)
    animation->rows = 1;
  if (animation->rows > GENIE_MAX_ROWS)
    animation->rows = GENIE_MAX_ROWS;
  if (!os_genie_begin(
          &animation->model, &snapshot, &animation->target, options->window.x,
          options->window.y, options->window.width, options->window.height,
          (uint64_t)options->duration_ms * 1000000ull,
          options->minimizing ? OS_GENIE_MINIMIZE : OS_GENIE_RESTORE)) {
    release_texture(animation);
    free(animation);
    return NULL;
  }
  if (!os_genie_step(&animation->model, 0, &animation->target, &animation->mesh,
                     animation->columns, animation->rows)) {
    destroy_animation(animation);
    return NULL;
  }
  clock_gettime(CLOCK_MONOTONIC, &animation->last_tick);
  animation->timer =
      wl_event_loop_add_timer(options->event_loop, animation_tick, animation);
  if (animation->timer == NULL) {
    destroy_animation(animation);
    return NULL;
  }
  wl_event_source_timer_update(animation->timer, 16);
  wlr_output_schedule_frame(options->output);
  return animation;
}

void tinywl_genie_cancel(struct tinywl_genie_animation *animation) {
  if (animation != NULL) {
    os_genie_cancel(&animation->model);
    destroy_animation(animation);
  }
}

void tinywl_genie_reverse(struct tinywl_genie_animation *animation) {
  if (animation != NULL) {
    os_genie_reverse(&animation->model);
    clock_gettime(CLOCK_MONOTONIC, &animation->last_tick);
  }
}

double tinywl_genie_progress(const struct tinywl_genie_animation *animation) {
  return animation == NULL ? 0.0 : animation->model.progress;
}

bool tinywl_genie_render(struct tinywl_genie_animation *animation,
                         struct wlr_render_pass *pass) {
  return animation != NULL && animation->texture != NULL &&
         os_vk_pass_add_mesh(pass, &animation->mesh, animation->texture, 1.0f);
}

bool tinywl_genie_targets(const struct tinywl_genie_animation *animation,
                          uint64_t target_id, uint64_t generation) {
  return animation != NULL && animation->target.id == target_id &&
         animation->target.generation == generation;
}

bool tinywl_genie_uses_output(const struct tinywl_genie_animation *animation,
                              const struct wlr_output *output) {
  return animation != NULL && animation->options.output == output;
}

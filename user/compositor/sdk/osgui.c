/*
 * Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#include "osgui.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define OSGUI_INITIAL_COMMAND_CAPACITY 16u

struct osgui_context {
  bool frame_active;
  struct osgui_theme theme;
  struct osgui_frame *active_frame;
};

struct osgui_frame {
  struct osgui_context *context;
  int width;
  int height;
  struct osgui_theme theme;
  struct osgui_draw_command *commands;
  size_t command_count;
  size_t command_capacity;
};

static struct osgui_theme default_theme(void) {
  return (struct osgui_theme){.size = sizeof(struct osgui_theme),
                              .version = OSGUI_API_VERSION,
                              .background_rgba = 0x20242aff,
                              .foreground_rgba = 0xf4f5f6ff,
                              .border_rgba = 0xffffff2e,
                              .accent_rgba = 0x3291ffff,
                              .corner_radius = 12.0f,
                              .border_width = 1.0f,
                              .titlebar_height = 34.0f};
}

static bool valid_rect(const struct osgui_rect *rect) {
  return rect != NULL && isfinite(rect->x) && isfinite(rect->y) &&
         isfinite(rect->width) && isfinite(rect->height) &&
         rect->width >= 0.0f && rect->height >= 0.0f;
}

enum osgui_result
osgui_context_create(const struct osgui_context_options *options,
                     osgui_context **out) {
  if (out != NULL)
    *out = NULL;
  if (out == NULL || options == NULL || options->size != sizeof(*options))
    return OSGUI_INVALID_ARGUMENT;
  if (options->version != OSGUI_API_VERSION)
    return OSGUI_UNSUPPORTED_VERSION;
  if (options->flags != 0)
    return OSGUI_INVALID_ARGUMENT;
  osgui_context *context = calloc(1, sizeof(*context));
  if (context == NULL)
    return OSGUI_OUT_OF_MEMORY;
  context->theme = default_theme();
  *out = context;
  return OSGUI_OK;
}

enum osgui_result
osgui_wayland_vulkan_create(osgui_context *context, struct wl_display *display,
                            struct wl_surface *surface,
                            const struct osgui_wayland_options *options) {
  if (context == NULL || display == NULL || surface == NULL ||
      options == NULL || options->size != sizeof(*options))
    return OSGUI_INVALID_ARGUMENT;
  if (options->version != OSGUI_API_VERSION)
    return OSGUI_UNSUPPORTED_VERSION;
  if (options->flags != 0 || context->frame_active)
    return OSGUI_INVALID_STATE;
  return OSGUI_BACKEND_UNAVAILABLE;
}

enum osgui_result osgui_begin_frame(osgui_context *context, int width,
                                    int height, osgui_frame **out) {
  if (out != NULL)
    *out = NULL;
  if (context == NULL || out == NULL || width <= 0 || height <= 0 ||
      width > OSGUI_MAX_SURFACE_DIMENSION ||
      height > OSGUI_MAX_SURFACE_DIMENSION)
    return OSGUI_INVALID_ARGUMENT;
  if (context->frame_active)
    return OSGUI_INVALID_STATE;
  osgui_frame *frame = calloc(1, sizeof(*frame));
  if (frame == NULL)
    return OSGUI_OUT_OF_MEMORY;
  frame->context = context;
  frame->width = width;
  frame->height = height;
  frame->theme = context->theme;
  frame->command_capacity = OSGUI_INITIAL_COMMAND_CAPACITY;
  frame->commands = calloc(frame->command_capacity, sizeof(*frame->commands));
  if (frame->commands == NULL) {
    free(frame);
    return OSGUI_OUT_OF_MEMORY;
  }
  context->frame_active = true;
  context->active_frame = frame;
  *out = frame;
  return OSGUI_OK;
}

static bool valid_theme(const struct osgui_theme *theme) {
  return theme != NULL && theme->size == sizeof(*theme) &&
         theme->version == OSGUI_API_VERSION &&
         isfinite(theme->corner_radius) && isfinite(theme->border_width) &&
         isfinite(theme->titlebar_height) && theme->corner_radius >= 0.0f &&
         theme->border_width >= 0.0f && theme->titlebar_height >= 0.0f;
}

enum osgui_result osgui_context_set_theme(osgui_context *context,
                                          const struct osgui_theme *theme) {
  if (context == NULL || !valid_theme(theme))
    return OSGUI_INVALID_ARGUMENT;
  if (context->frame_active)
    return OSGUI_INVALID_STATE;
  context->theme = *theme;
  return OSGUI_OK;
}

enum osgui_result osgui_context_theme_snapshot(const osgui_context *context,
                                               struct osgui_theme *theme) {
  if (context == NULL || theme == NULL || theme->size != sizeof(*theme))
    return OSGUI_INVALID_ARGUMENT;
  *theme = context->theme;
  return OSGUI_OK;
}

static enum osgui_result
append_command(osgui_frame *frame, const struct osgui_draw_command *command) {
  if (frame->command_count == frame->command_capacity) {
    if (frame->command_capacity > SIZE_MAX / 2 / sizeof(*frame->commands))
      return OSGUI_OUT_OF_MEMORY;
    size_t capacity = frame->command_capacity * 2;
    void *commands =
        realloc(frame->commands, capacity * sizeof(*frame->commands));
    if (commands == NULL)
      return OSGUI_OUT_OF_MEMORY;
    frame->commands = commands;
    frame->command_capacity = capacity;
  }
  frame->commands[frame->command_count++] = *command;
  return OSGUI_OK;
}

enum osgui_result
osgui_draw_rounded_rect(osgui_frame *frame, const struct osgui_rect *rect,
                        const struct osgui_rect_style *style) {
  if (frame == NULL || !valid_rect(rect) || style == NULL ||
      style->size != sizeof(*style) || !isfinite(style->radius) ||
      !isfinite(style->border_width) || style->radius < 0.0f ||
      style->border_width < 0.0f)
    return OSGUI_INVALID_ARGUMENT;
  if (style->radius > fminf(rect->width, rect->height) * 0.5f ||
      style->border_width > fminf(rect->width, rect->height) * 0.5f)
    return OSGUI_INVALID_ARGUMENT;
  struct osgui_draw_command command = {
      .size = sizeof(command),
      .type = OSGUI_COMMAND_ROUNDED_RECT,
      .rounded_rect = {.rect = *rect, .style = *style}};
  return append_command(frame, &command);
}

enum osgui_result
osgui_draw_window_chrome(osgui_frame *frame,
                         const struct osgui_window_chrome *chrome) {
  if (frame == NULL || chrome == NULL || chrome->size != sizeof(*chrome) ||
      !valid_rect(&chrome->bounds))
    return OSGUI_INVALID_ARGUMENT;
  struct osgui_draw_command command = {.size = sizeof(command),
                                       .type = OSGUI_COMMAND_WINDOW_CHROME,
                                       .chrome = *chrome};
  return append_command(frame, &command);
}

enum osgui_result osgui_end_frame(osgui_frame *frame) {
  if (frame == NULL || frame->context == NULL ||
      !frame->context->frame_active || frame->context->active_frame != frame)
    return OSGUI_INVALID_ARGUMENT;
  frame->context->frame_active = false;
  frame->context->active_frame = NULL;
  free(frame->commands);
  free(frame);
  return OSGUI_OK;
}

size_t osgui_frame_command_count(const osgui_frame *frame) {
  return frame == NULL ? 0 : frame->command_count;
}

const struct osgui_draw_command *osgui_frame_command(const osgui_frame *frame,
                                                     size_t index) {
  return frame == NULL || index >= frame->command_count
             ? NULL
             : &frame->commands[index];
}

void osgui_cancel_frame(osgui_frame *frame) {
  if (frame == NULL)
    return;
  if (frame->context != NULL && frame->context->active_frame == frame) {
    frame->context->frame_active = false;
    frame->context->active_frame = NULL;
  }
  free(frame->commands);
  free(frame);
}

void osgui_context_destroy(osgui_context *context) {
  if (context == NULL)
    return;
  if (context->active_frame != NULL) {
    context->active_frame->context = NULL;
    free(context->active_frame->commands);
    free(context->active_frame);
  }
  free(context);
}

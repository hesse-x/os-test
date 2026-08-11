/* Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#include "osgui.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>

struct osgui_context {
  bool frame_active;
};

struct osgui_frame {
  struct osgui_context *context;
  int width;
  int height;
};

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
  context->frame_active = true;
  *out = frame;
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
  return OSGUI_OK;
}

enum osgui_result
osgui_draw_window_chrome(osgui_frame *frame,
                         const struct osgui_window_chrome *chrome) {
  if (frame == NULL || chrome == NULL || chrome->size != sizeof(*chrome) ||
      !valid_rect(&chrome->bounds))
    return OSGUI_INVALID_ARGUMENT;
  return OSGUI_OK;
}

enum osgui_result osgui_end_frame(osgui_frame *frame) {
  if (frame == NULL || frame->context == NULL || !frame->context->frame_active)
    return OSGUI_INVALID_ARGUMENT;
  frame->context->frame_active = false;
  free(frame);
  return OSGUI_OK;
}

void osgui_context_destroy(osgui_context *context) {
  if (context != NULL)
    free(context);
}

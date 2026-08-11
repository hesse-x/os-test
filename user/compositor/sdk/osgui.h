/* Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#ifndef OSGUI_H
#define OSGUI_H

#include <stdint.h>

#define OSGUI_API_VERSION 1u
#define OSGUI_MAX_SURFACE_DIMENSION 16384

typedef struct osgui_context osgui_context;
typedef struct osgui_frame osgui_frame;
struct wl_display;
struct wl_surface;

enum osgui_result {
  OSGUI_OK,
  OSGUI_INVALID_ARGUMENT,
  OSGUI_UNSUPPORTED_VERSION,
  OSGUI_INVALID_STATE,
  OSGUI_OUT_OF_MEMORY,
  OSGUI_BACKEND_UNAVAILABLE,
};

struct osgui_context_options {
  uint32_t size;
  uint32_t version;
  uint32_t flags;
};

struct osgui_wayland_options {
  uint32_t size;
  uint32_t version;
  uint32_t flags;
};

struct osgui_rect {
  float x, y, width, height;
};

struct osgui_rect_style {
  uint32_t size;
  float radius;
  float border_width;
  uint32_t fill_rgba;
  uint32_t border_rgba;
};

struct osgui_window_chrome {
  uint32_t size;
  struct osgui_rect bounds;
  uint32_t flags;
};

enum osgui_result
osgui_context_create(const struct osgui_context_options *options,
                     osgui_context **out);
enum osgui_result
osgui_wayland_vulkan_create(osgui_context *context, struct wl_display *display,
                            struct wl_surface *surface,
                            const struct osgui_wayland_options *options);
enum osgui_result osgui_begin_frame(osgui_context *context, int width,
                                    int height, osgui_frame **out);
enum osgui_result osgui_draw_rounded_rect(osgui_frame *frame,
                                          const struct osgui_rect *rect,
                                          const struct osgui_rect_style *style);
enum osgui_result
osgui_draw_window_chrome(osgui_frame *frame,
                         const struct osgui_window_chrome *chrome);
enum osgui_result osgui_end_frame(osgui_frame *frame);
void osgui_context_destroy(osgui_context *context);

#endif

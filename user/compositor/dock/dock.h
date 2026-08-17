/*
 * Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#ifndef OS_COMPOSITOR_DOCK_H
#define OS_COMPOSITOR_DOCK_H

#include <stdbool.h>

#include "dock_layout.h"

struct wl_compositor;
struct wl_output;
struct wl_surface;
struct zwlr_layer_shell_v1;
struct zwlr_layer_surface_v1;

struct os_dock {
  struct wl_surface *surface;
  struct zwlr_layer_surface_v1 *layer_surface;
  struct os_dock_layout layout;
  bool configured;
  void (*render)(void *data);
  void (*closed)(void *data);
  void *callback_data;
};

bool os_dock_create(struct os_dock *dock, struct wl_compositor *compositor,
                    struct zwlr_layer_shell_v1 *layer_shell,
                    struct wl_output *output, int output_width,
                    int output_height, int scale, void (*render)(void *data),
                    void (*closed)(void *data), void *data);
void os_dock_destroy(struct os_dock *dock);
bool os_dock_owns_surface(const struct os_dock *dock,
                          const struct wl_surface *surface);

#endif

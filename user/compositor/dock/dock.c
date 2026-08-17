/*
 * Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#include "dock.h"

#include <string.h>
#include <wayland-client.h>

#include "wlr-layer-shell-client-protocol.h"

static void dock_configure(void *data, struct zwlr_layer_surface_v1 *layer,
                           uint32_t serial, uint32_t width, uint32_t height) {
  struct os_dock *dock = data;
  (void)width;
  (void)height;
  zwlr_layer_surface_v1_ack_configure(layer, serial);
  if (!dock->configured) {
    dock->configured = true;
    if (dock->render != NULL)
      dock->render(dock->callback_data);
  }
}

static void dock_closed(void *data, struct zwlr_layer_surface_v1 *layer) {
  struct os_dock *dock = data;
  (void)layer;
  if (dock->closed != NULL)
    dock->closed(dock->callback_data);
}

static const struct zwlr_layer_surface_v1_listener dock_listener = {
    .configure = dock_configure,
    .closed = dock_closed,
};

bool os_dock_create(struct os_dock *dock, struct wl_compositor *compositor,
                    struct zwlr_layer_shell_v1 *layer_shell,
                    struct wl_output *output, int output_width,
                    int output_height, int scale, void (*render)(void *data),
                    void (*closed)(void *data), void *data) {
  if (dock == NULL || compositor == NULL || layer_shell == NULL ||
      output == NULL || render == NULL ||
      !os_dock_layout_init(&dock->layout, output_width, output_height, scale))
    return false;
  dock->render = render;
  dock->closed = closed;
  dock->callback_data = data;
  dock->surface = wl_compositor_create_surface(compositor);
  if (dock->surface == NULL)
    return false;
  dock->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
      layer_shell, dock->surface, output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
      "desktop-dock");
  if (dock->layer_surface == NULL) {
    wl_surface_destroy(dock->surface);
    dock->surface = NULL;
    return false;
  }
  wl_surface_set_user_data(dock->surface, dock);
  wl_surface_set_buffer_scale(dock->surface, dock->layout.scale);
  zwlr_layer_surface_v1_set_size(dock->layer_surface, dock->layout.width,
                                 dock->layout.height);
  zwlr_layer_surface_v1_set_anchor(dock->layer_surface,
                                   ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM);
  zwlr_layer_surface_v1_set_margin(dock->layer_surface, 0, 0, 0, 0);
  zwlr_layer_surface_v1_set_exclusive_zone(dock->layer_surface, 0);
  zwlr_layer_surface_v1_add_listener(dock->layer_surface, &dock_listener, dock);
  wl_surface_commit(dock->surface);
  return true;
}

void os_dock_destroy(struct os_dock *dock) {
  if (dock == NULL)
    return;
  if (dock->layer_surface != NULL)
    zwlr_layer_surface_v1_destroy(dock->layer_surface);
  if (dock->surface != NULL)
    wl_surface_destroy(dock->surface);
  memset(dock, 0, sizeof(*dock));
}

bool os_dock_owns_surface(const struct os_dock *dock,
                          const struct wl_surface *surface) {
  return dock != NULL && dock->surface != NULL && dock->surface == surface;
}

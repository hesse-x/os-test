/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/input-event-codes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/libinput.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
#include <xos/perf.h>
#include <xos/syscall_nums.h>

#include "core/frame.h"
#include "core/lifetime.h"
#include "core/server.h"
#include "cursor.h"
#include "protocols/effect_server.h"
#include "protocols/effect_state.h"
#include "renderer/vulkan/os_vk_renderer.h"
#include "renderer/vulkan/prime_probe.h"
#include "window/animation/genie_mesh.h"
#include "window/animation/genie_runtime.h"
#include "window/window_state.h"

#define SSD_TITLE_HEIGHT 32
#define SSD_BORDER_WIDTH 2
#define SSD_BUTTON_SIZE 16
#define SSD_BUTTON_CLOSE_X 12
#define SSD_BUTTON_MINIMIZE_X 34
#define SSD_BUTTON_MAXIMIZE_X 56
#define SSD_BUTTON_HIT_LEFT 4
#define SSD_BUTTON_HIT_CLOSE_END 31
#define SSD_BUTTON_HIT_MINIMIZE_END 53
#define SSD_BUTTON_HIT_RIGHT 80
#define TINYWL_POINTER_ACCEL_DEFAULT 0.15
#define GENIE_DURATION_MS 300

/* wlroots keeps this allocator constructor private, but exports the symbol. */
struct wlr_allocator *wlr_udmabuf_allocator_create(void);

/* For brevity's sake, struct members are annotated where they are used. */
enum tinywl_cursor_mode {
  TINYWL_CURSOR_PASSTHROUGH,
  TINYWL_CURSOR_MOVE,
  TINYWL_CURSOR_RESIZE,
};

struct tinywl_server {
  struct wl_display *wl_display;
  struct wlr_backend *backend;
  struct wlr_renderer *renderer;
  struct wlr_allocator *allocator;
  struct wlr_scene *scene;
  struct wlr_scene_output_layout *scene_layout;
  struct wlr_scene_tree *layer_background;
  struct wlr_scene_tree *layer_bottom;
  struct wlr_scene_tree *layer_toplevel;
  struct wlr_scene_tree *layer_overlay;

  struct wlr_layer_shell_v1 *layer_shell;
  struct wl_listener new_layer_surface;
  struct wlr_xdg_decoration_manager_v1 *decoration_manager;
  struct wl_listener new_decoration;

  struct wlr_xdg_shell *xdg_shell;
  struct wl_listener new_xdg_toplevel;
  struct wl_listener new_xdg_popup;
  struct wl_list toplevels;

  struct wlr_cursor *cursor;
  struct wlr_xcursor_manager *cursor_mgr;
  struct wl_listener cursor_motion;
  struct wl_listener cursor_motion_absolute;
  struct wl_listener cursor_button;
  struct wl_listener cursor_axis;
  struct wl_listener cursor_frame;

  struct wlr_seat *seat;
  struct wl_listener new_input;
  struct wl_listener request_cursor;
  struct wl_listener request_set_selection;
  struct wl_list keyboards;
  enum tinywl_cursor_mode cursor_mode;
  struct tinywl_toplevel *grabbed_toplevel;
  double grab_x, grab_y;
  struct wlr_box grab_geobox;
  uint32_t resize_edges;
  bool consume_dock_click;

  struct wlr_output_layout *output_layout;
  struct wl_list outputs;
  struct wl_list layer_surfaces;
  struct wl_listener new_output;
  struct os_lifetime_counters lifetime;
  struct os_effect_server *effect_server;
};

struct tinywl_output {
  struct wl_list link;
  struct tinywl_server *server;
  struct wlr_output *wlr_output;
  struct wl_listener frame;
  struct wl_listener request_state;
  struct wl_listener destroy;
};

struct tinywl_toplevel {
  struct wl_list link;
  struct tinywl_server *server;
  struct wlr_xdg_toplevel *xdg_toplevel;
  struct wlr_scene_tree *scene_tree;
  struct wlr_scene_tree *content_tree;
  struct wlr_scene_tree *decoration_tree;
  struct wlr_scene_rect *titlebar;
  struct wlr_scene_rect *border_left;
  struct wlr_scene_rect *border_right;
  struct wlr_scene_rect *border_bottom;
  struct wlr_scene_tree *button_close;
  struct wlr_scene_tree *button_minimize;
  struct wlr_scene_tree *button_maximize;
  int content_width, content_height;
  bool maximized;
  bool mapped;
  struct os_window_state_machine state;
  struct tinywl_genie_animation *animation;
  int restore_x, restore_y, restore_width, restore_height;
  struct wl_listener map;
  struct wl_listener unmap;
  struct wl_listener commit;
  struct wl_listener destroy;
  struct wl_listener request_move;
  struct wl_listener request_resize;
  struct wl_listener request_maximize;
  struct wl_listener request_minimize;
  struct wl_listener request_fullscreen;
};

struct tinywl_layer_surface {
  struct wl_list link;
  struct tinywl_server *server;
  struct wlr_layer_surface_v1 *layer_surface;
  struct wlr_scene_layer_surface_v1 *scene_layer;
  struct wl_listener commit;
  struct wl_listener destroy;
  uint64_t target_generation;
};

static bool is_output_dock(const struct tinywl_layer_surface *surface,
                           const struct wlr_output *output) {
  const struct wlr_layer_surface_v1 *layer = surface->layer_surface;
  return layer->output == output && layer->namespace != NULL &&
         strcmp(layer->namespace, "desktop-dock") == 0;
}

static void set_output_dock_enabled(struct tinywl_output *output,
                                    bool enabled) {
  struct tinywl_layer_surface *surface;
  wl_list_for_each(surface, &output->server->layer_surfaces, link) {
    if (is_output_dock(surface, output->wlr_output))
      wlr_scene_node_set_enabled(&surface->scene_layer->tree->node, enabled);
  }
}

static bool add_dock_rect(struct wlr_render_pass *pass, float x, float y,
                          float width, float height, float radius,
                          const float color[4]) {
  struct os_vk_rounded_rect rect = {
      .x = x,
      .y = y,
      .width = width,
      .height = height,
      .radius = radius,
  };
  memcpy(rect.fill, color, sizeof(rect.fill));
  return os_vk_pass_add_rounded_rect(pass, &rect);
}

static bool render_terminal_dock_icon(struct wlr_render_pass *pass, float x,
                                      float y, float width, float height,
                                      float scale) {
  const float shell[] = {0.69f, 0.72f, 0.76f, 1.0f};
  const float screen[] = {0.009f, 0.010f, 0.014f, 1.0f};
  const float glyph[] = {0.91f, 0.93f, 0.95f, 1.0f};
  const float indicator[] = {0.023f, 0.025f, 0.030f, 1.0f};
  float icon = width * 8.0f / 13.0f;
  float padding = (width - icon) * 0.5f;
  float top = height / 10.0f;
  float icon_x = x + padding;
  float icon_y = y + top + padding;
  float inset = fmaxf(icon / 16.0f, 2.0f * scale);
  float mark = fmaxf(icon / 24.0f, scale);
  if (!add_dock_rect(pass, icon_x, icon_y, icon, icon, icon / 4.0f, shell) ||
      !add_dock_rect(pass, icon_x + inset, icon_y + inset, icon - 2.0f * inset,
                     icon - 2.0f * inset, icon / 5.0f, screen))
    return false;

  float glyph_x = icon_x + icon / 4.0f;
  float glyph_y = icon_y + icon / 3.0f;
  int steps = (int)(icon / 6.0f);
  for (int i = 0; i < steps; ++i) {
    if (!add_dock_rect(pass, glyph_x + i * mark, glyph_y + i * mark * 0.5f,
                       mark, mark, mark * 0.25f, glyph) ||
        !add_dock_rect(pass, glyph_x + i * mark,
                       glyph_y + icon / 5.0f - i * mark * 0.5f, mark, mark,
                       mark * 0.25f, glyph))
      return false;
  }
  if (!add_dock_rect(pass, icon_x + icon * 0.5f, icon_y + icon * 0.6f,
                     icon * 0.25f, mark, mark * 0.5f, glyph))
    return false;
  float indicator_h = fmaxf(height / 24.0f, 2.0f * scale);
  float indicator_w = height / 9.0f;
  return add_dock_rect(pass, x + (width - indicator_w) * 0.5f,
                       y + height - indicator_h - 2.0f * scale, indicator_w,
                       indicator_h, indicator_h * 0.5f, indicator);
}

static bool render_system_overlays(struct wlr_render_pass *pass, void *data) {
  struct tinywl_output *output = data;
  struct tinywl_toplevel *toplevel;
  wl_list_for_each(toplevel, &output->server->toplevels, link) {
    if (tinywl_genie_uses_output(toplevel->animation, output->wlr_output) &&
        !tinywl_genie_render(toplevel->animation, pass))
      return false;
  }
  struct tinywl_layer_surface *surface;
  wl_list_for_each(surface, &output->server->layer_surfaces, link) {
    struct wlr_layer_surface_v1 *layer = surface->layer_surface;
    if (!is_output_dock(surface, output->wlr_output) ||
        layer->current.actual_width == 0 || layer->current.actual_height == 0)
      continue;
    // The scene pass excludes the dock so backdrop blur never samples the
    // dock's own icon. Re-enable it now for foreground rendering and input.
    wlr_scene_node_set_enabled(&surface->scene_layer->tree->node, true);
    int x, y;
    if (!wlr_scene_node_coords(&surface->scene_layer->tree->node, &x, &y))
      continue;
    struct wlr_box output_box;
    wlr_output_layout_get_box(output->server->output_layout, output->wlr_output,
                              &output_box);
    float scale = output->wlr_output->scale;
    struct os_vk_rounded_rect rect = {
        .x = (x - output_box.x) * scale,
        .y = (y - output_box.y) * scale,
        .width = layer->current.actual_width * scale,
        .height = layer->current.actual_height * scale,
        .radius = 18.0f * scale,
        .border_width = 1.0f * scale,
        .fill = {0.0f, 0.0f, 0.0f, 0.0f},
        .border = {0.82f, 0.84f, 0.87f, 0.14f},
    };
    struct os_effect_value effect;
    if (os_effect_server_current(output->server->effect_server, layer->surface,
                                 &effect)) {
      if (effect.width > 0 && effect.height > 0) {
        rect.x += effect.x * scale;
        rect.y += effect.y * scale;
        rect.width = effect.width * scale;
        rect.height = effect.height * scale;
      }
      uint32_t tint = effect.tint_rgba8;
      struct os_vk_backdrop_blur blur = {
          .x = rect.x,
          .y = rect.y,
          .width = rect.width,
          .height = rect.height,
          .radius = effect.radius,
          .downsample = 4,
          .corner_radius = rect.radius,
          .tint = {((tint >> 24) & 0xff) / 255.0f,
                   ((tint >> 16) & 0xff) / 255.0f,
                   ((tint >> 8) & 0xff) / 255.0f, (tint & 0xff) / 255.0f},
          .opacity = 0.62f,
      };
      if (effect.enabled) {
        if (!os_vk_pass_add_backdrop_blur(pass, &blur, NULL))
          return false;
      }
    }
    if (!os_vk_pass_add_rounded_rect(pass, &rect))
      return false;
    float dock_x = (x - output_box.x) * scale;
    float dock_y = (y - output_box.y) * scale;
    if (!render_terminal_dock_icon(pass, dock_x, dock_y,
                                   layer->current.actual_width * scale,
                                   layer->current.actual_height * scale, scale))
      return false;
  }
  return true;
}

struct tinywl_popup {
  struct tinywl_server *server;
  struct wlr_xdg_popup *xdg_popup;
  struct wl_listener commit;
  struct wl_listener destroy;
};

struct tinywl_keyboard {
  struct wl_list link;
  struct tinywl_server *server;
  struct wlr_keyboard *wlr_keyboard;

  struct wl_listener modifiers;
  struct wl_listener key;
  struct wl_listener destroy;
};

static void begin_interactive(struct tinywl_toplevel *toplevel,
                              enum tinywl_cursor_mode mode, uint32_t edges);

static void update_ssd(struct tinywl_toplevel *toplevel) {
  /* wlroots 0.20: geometry is a plain field (no more
   * wlr_xdg_surface_get_geometry()). */
  struct wlr_box geo = toplevel->xdg_toplevel->base->geometry;
  if (geo.width <= 0 || geo.height <= 0) {
    return;
  }
  toplevel->content_width = geo.width;
  toplevel->content_height = geo.height;

  wlr_scene_rect_set_size(toplevel->titlebar, geo.width, SSD_TITLE_HEIGHT);
  wlr_scene_node_set_position(&toplevel->titlebar->node, 0, -SSD_TITLE_HEIGHT);
  wlr_scene_rect_set_size(toplevel->border_left, SSD_BORDER_WIDTH,
                          geo.height + SSD_TITLE_HEIGHT + SSD_BORDER_WIDTH);
  wlr_scene_node_set_position(&toplevel->border_left->node, -SSD_BORDER_WIDTH,
                              -SSD_TITLE_HEIGHT);
  wlr_scene_rect_set_size(toplevel->border_right, SSD_BORDER_WIDTH,
                          geo.height + SSD_TITLE_HEIGHT + SSD_BORDER_WIDTH);
  wlr_scene_node_set_position(&toplevel->border_right->node, geo.width,
                              -SSD_TITLE_HEIGHT);
  wlr_scene_rect_set_size(toplevel->border_bottom, geo.width, SSD_BORDER_WIDTH);
  wlr_scene_node_set_position(&toplevel->border_bottom->node, 0, geo.height);

  const int button_y = -SSD_TITLE_HEIGHT / 2 - SSD_BUTTON_SIZE / 2;
  wlr_scene_node_set_position(&toplevel->button_close->node, SSD_BUTTON_CLOSE_X,
                              button_y);
  wlr_scene_node_set_position(&toplevel->button_minimize->node,
                              SSD_BUTTON_MINIMIZE_X, button_y);
  wlr_scene_node_set_position(&toplevel->button_maximize->node,
                              SSD_BUTTON_MAXIMIZE_X, button_y);
  if (toplevel->xdg_toplevel->base->surface->mapped) {
    wlr_scene_node_set_enabled(&toplevel->decoration_tree->node, true);
  }
}

enum ssd_button_icon {
  SSD_ICON_CLOSE,
  SSD_ICON_MINIMIZE,
  SSD_ICON_MAXIMIZE,
};

static int ssd_button_at(double x, double y) {
  if (y >= 0 || y < -SSD_TITLE_HEIGHT || x < SSD_BUTTON_HIT_LEFT ||
      x >= SSD_BUTTON_HIT_RIGHT)
    return -1;
  if (x < SSD_BUTTON_HIT_CLOSE_END)
    return SSD_ICON_CLOSE;
  if (x < SSD_BUTTON_HIT_MINIMIZE_END)
    return SSD_ICON_MINIMIZE;
  return SSD_ICON_MAXIMIZE;
}

struct ssd_button_buffer {
  struct wlr_buffer base;
  uint32_t pixels[SSD_BUTTON_SIZE * SSD_BUTTON_SIZE];
};

static void ssd_button_buffer_destroy(struct wlr_buffer *wlr_buffer) {
  struct ssd_button_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
  wlr_buffer_finish(wlr_buffer);
  free(buffer);
}

static bool ssd_button_buffer_begin_access(struct wlr_buffer *wlr_buffer,
                                           uint32_t flags, void **data,
                                           uint32_t *format, size_t *stride) {
  struct ssd_button_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
  if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) {
    return false;
  }
  *data = buffer->pixels;
  *format = DRM_FORMAT_ARGB8888;
  *stride = SSD_BUTTON_SIZE * sizeof(uint32_t);
  return true;
}

static void ssd_button_buffer_end_access(struct wlr_buffer *wlr_buffer) {
  (void)wlr_buffer;
}

static const struct wlr_buffer_impl ssd_button_buffer_impl = {
    .destroy = ssd_button_buffer_destroy,
    .begin_data_ptr_access = ssd_button_buffer_begin_access,
    .end_data_ptr_access = ssd_button_buffer_end_access,
};

static float absf(float value) { return value < 0 ? -value : value; }

static bool ssd_icon_contains(enum ssd_button_icon icon, float x, float y) {
  if (icon == SSD_ICON_CLOSE) {
    return absf(x) <= 3.5f && absf(y) <= 3.5f &&
           (absf(x - y) <= 0.72f || absf(x + y) <= 0.72f);
  }
  if (icon == SSD_ICON_MINIMIZE) {
    return absf(x) <= 3.5f && absf(y) <= 0.68f;
  }
  return (absf(x) <= 3.5f && absf(y) <= 0.68f) ||
         (absf(y) <= 3.5f && absf(x) <= 0.68f);
}

static struct ssd_button_buffer *
create_ssd_button_buffer(const float color[static 4],
                         enum ssd_button_icon icon) {
  struct ssd_button_buffer *buffer = calloc(1, sizeof(*buffer));
  if (buffer == NULL) {
    return NULL;
  }
  wlr_buffer_init(&buffer->base, &ssd_button_buffer_impl, SSD_BUTTON_SIZE,
                  SSD_BUTTON_SIZE);

  const uint8_t base[3] = {(uint8_t)(color[0] * 255.0f),
                           (uint8_t)(color[1] * 255.0f),
                           (uint8_t)(color[2] * 255.0f)};
  const uint8_t marks[3][3] = {
      {104, 31, 25},
      {105, 72, 8},
      {12, 78, 24},
  };
  const uint8_t *mark = marks[icon];
  const int samples = 4;
  const float center = SSD_BUTTON_SIZE / 2.0f;
  const float radius_sq = 7.25f * 7.25f;

  /* Supersampling turns both the circle edge and symbols into coverage masks.
   */
  for (int py = 0; py < SSD_BUTTON_SIZE; py++) {
    for (int px = 0; px < SSD_BUTTON_SIZE; px++) {
      unsigned int a = 0, r = 0, g = 0, b = 0;
      for (int sy = 0; sy < samples; sy++) {
        for (int sx = 0; sx < samples; sx++) {
          float x = px + (sx + 0.5f) / samples - center;
          float y = py + (sy + 0.5f) / samples - center;
          if (x * x + y * y > radius_sq) {
            continue;
          }
          const uint8_t *sample = ssd_icon_contains(icon, x, y) ? mark : base;
          a += 255;
          r += sample[0];
          g += sample[1];
          b += sample[2];
        }
      }
      unsigned int divisor = samples * samples;
      buffer->pixels[py * SSD_BUTTON_SIZE + px] =
          ((a / divisor) << 24) | ((r / divisor) << 16) | ((g / divisor) << 8) |
          (b / divisor);
    }
  }
  return buffer;
}

static struct wlr_scene_tree *create_ssd_button(struct wlr_scene_tree *parent,
                                                const float color[static 4],
                                                enum ssd_button_icon icon) {
  struct wlr_scene_tree *button = wlr_scene_tree_create(parent);
  struct ssd_button_buffer *buffer = create_ssd_button_buffer(color, icon);
  if (buffer != NULL) {
    wlr_scene_buffer_create(button, &buffer->base);
    wlr_buffer_drop(&buffer->base);
  }
  return button;
}

static void focus_toplevel(struct tinywl_toplevel *toplevel,
                           struct wlr_surface *surface) {
  /* Note: this function only deals with keyboard focus. */
  if (toplevel == NULL) {
    return;
  }
  struct tinywl_server *server = toplevel->server;
  struct wlr_seat *seat = server->seat;
  struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
  if (prev_surface == surface) {
    /* Don't re-focus an already focused surface. */
    return;
  }
  if (prev_surface) {
    /*
     * Deactivate the previously focused surface. This lets the client know
     * it no longer has focus and the client will repaint accordingly, e.g.
     * stop displaying a caret.
     */
    struct wlr_xdg_toplevel *prev_toplevel =
        wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
    if (prev_toplevel != NULL) {
      wlr_xdg_toplevel_set_activated(prev_toplevel, false);
    }
  }
  struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
  /* Move the toplevel to the front */
  wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
  wl_list_remove(&toplevel->link);
  wl_list_insert(&server->toplevels, &toplevel->link);
  /* Activate the new surface */
  wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
  /*
   * Tell the seat to have the keyboard enter this surface. wlroots will keep
   * track of this and automatically send key events to the appropriate
   * clients without additional work on your part.
   */
  if (keyboard != NULL) {
    wlr_seat_keyboard_notify_enter(seat, toplevel->xdg_toplevel->base->surface,
                                   keyboard->keycodes, keyboard->num_keycodes,
                                   &keyboard->modifiers);
  }
}

static void set_toplevel_maximized(struct tinywl_toplevel *toplevel,
                                   struct wlr_output *output, bool maximized) {
  if (output == NULL || toplevel->maximized == maximized)
    return;

  struct wlr_box box;
  wlr_output_layout_get_box(toplevel->server->output_layout, output, &box);
  if (maximized) {
    int x, y;
    wlr_scene_node_coords(&toplevel->scene_tree->node, &x, &y);
    toplevel->restore_x = x;
    toplevel->restore_y = y;
    toplevel->restore_width =
        toplevel->content_width > 0 ? toplevel->content_width : 720;
    toplevel->restore_height =
        toplevel->content_height > 0 ? toplevel->content_height : 460;
    wlr_scene_node_set_position(&toplevel->scene_tree->node,
                                box.x + SSD_BORDER_WIDTH,
                                box.y + SSD_TITLE_HEIGHT + SSD_BORDER_WIDTH);
    wlr_xdg_toplevel_set_size(
        toplevel->xdg_toplevel, box.width - 2 * SSD_BORDER_WIDTH,
        box.height - SSD_TITLE_HEIGHT - 2 * SSD_BORDER_WIDTH);
  } else {
    wlr_scene_node_set_position(&toplevel->scene_tree->node,
                                toplevel->restore_x, toplevel->restore_y);
    wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, toplevel->restore_width,
                              toplevel->restore_height);
  }
  toplevel->maximized = maximized;
  wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, maximized);
}

static void focus_next_visible_toplevel(struct tinywl_server *server) {
  struct tinywl_toplevel *candidate;
  wl_list_for_each(candidate, &server->toplevels, link) {
    if (candidate->mapped && os_window_is_visible(&candidate->state) &&
        candidate->animation == NULL) {
      focus_toplevel(candidate, candidate->xdg_toplevel->base->surface);
      return;
    }
  }
}

static bool dock_target_for_output(struct tinywl_server *server,
                                   struct wlr_output *output,
                                   struct os_genie_target *target) {
  struct tinywl_layer_surface *surface;
  wl_list_for_each(surface, &server->layer_surfaces, link) {
    struct wlr_layer_surface_v1 *layer = surface->layer_surface;
    if (layer->output != output || layer->namespace == NULL ||
        strcmp(layer->namespace, "desktop-dock") != 0)
      continue;
    struct wlr_box output_box;
    wlr_output_layout_get_box(server->output_layout, output, &output_box);
    int x, y;
    if (!wlr_scene_node_coords(&surface->scene_layer->tree->node, &x, &y))
      return false;
    float scale = output->scale > 0 ? output->scale : 1;
    float icon = 48.0f * scale;
    *target = (struct os_genie_target){
        .id = (uint64_t)(uintptr_t)surface,
        .generation = surface->target_generation,
        .x = (x - output_box.x + layer->current.actual_width * 0.5f) * scale -
             icon * 0.5f,
        .y = (y - output_box.y + layer->current.actual_height) * scale - 4,
        .width = icon,
        .height = 4,
    };
    return true;
  }
  return false;
}

static void genie_finished(void *data, bool minimizing) {
  struct tinywl_toplevel *toplevel = data;
  toplevel->animation = NULL;
  if (os_window_finish_animation(&toplevel->state) != OS_WINDOW_TRANSITION_OK) {
    wlr_log(WLR_ERROR, "window.finish invalid state=%d generation=%llu",
            toplevel->state.state,
            (unsigned long long)toplevel->state.generation);
    return;
  }
  if (minimizing) {
    focus_next_visible_toplevel(toplevel->server);
  } else {
    wlr_scene_node_set_enabled(&toplevel->scene_tree->node, true);
    focus_toplevel(toplevel, toplevel->xdg_toplevel->base->surface);
  }
  wlr_log(WLR_INFO, "genie.finish direction=%s",
          minimizing ? "minimize" : "restore");
}

static bool start_genie_animation(struct tinywl_toplevel *toplevel,
                                  struct wlr_output *output, bool minimizing) {
  if (toplevel->animation != NULL || !toplevel->mapped) {
    return false;
  }
  enum os_window_transition transition =
      minimizing ? os_window_request_minimize(&toplevel->state)
                 : os_window_request_restore(&toplevel->state);
  if (transition != OS_WINDOW_TRANSITION_OK)
    return false;
  if (output == NULL) {
    output =
        wlr_output_layout_get_center_output(toplevel->server->output_layout);
  }
  if (output == NULL || toplevel->content_width <= 0 ||
      toplevel->content_height <= 0) {
    os_window_cancel_animation(&toplevel->state);
    return false;
  }

  int tx, ty;
  wlr_scene_node_coords(&toplevel->scene_tree->node, &tx, &ty);
  struct wlr_box output_box;
  wlr_output_layout_get_box(toplevel->server->output_layout, output,
                            &output_box);
  float output_scale = output->scale > 0 ? output->scale : 1.0f;
  struct os_genie_target dock_target;
  bool has_dock_target =
      dock_target_for_output(toplevel->server, output, &dock_target);
  struct wlr_scene_rect *ssd_rects[] = {
      toplevel->titlebar,
      toplevel->border_left,
      toplevel->border_right,
      toplevel->border_bottom,
  };
  struct tinywl_genie_options options = {
      .event_loop = wl_display_get_event_loop(toplevel->server->wl_display),
      .renderer = toplevel->server->renderer,
      .output = output,
      .source = &toplevel->scene_tree->node,
      /* scene_tree traversal already reports layout-global coordinates. */
      .source_x = 0,
      .source_y = 0,
      .rects = ssd_rects,
      .rect_count = sizeof(ssd_rects) / sizeof(ssd_rects[0]),
      .snapshot_window =
          {
              .x = tx - SSD_BORDER_WIDTH,
              .y = ty - SSD_TITLE_HEIGHT,
              .width = toplevel->content_width + 2 * SSD_BORDER_WIDTH,
              .height = toplevel->content_height + SSD_TITLE_HEIGHT +
                        SSD_BORDER_WIDTH,
          },
      .window =
          {
              .x = (int)lroundf((tx - output_box.x - SSD_BORDER_WIDTH) *
                                output_scale),
              .y = (int)lroundf((ty - output_box.y - SSD_TITLE_HEIGHT) *
                                output_scale),
              .width = (int)lroundf(
                  (toplevel->content_width + 2 * SSD_BORDER_WIDTH) *
                  output_scale),
              .height = (int)lroundf((toplevel->content_height +
                                      SSD_TITLE_HEIGHT + SSD_BORDER_WIDTH) *
                                     output_scale),
          },
      .target_x = has_dock_target ? dock_target.x + dock_target.width * 0.5
                                  : output_box.width * output_scale * 0.5,
      .target_y = has_dock_target ? dock_target.y + dock_target.height * 0.5
                                  : (output_box.height - 32) * output_scale,
      .target_width = has_dock_target ? dock_target.width : 48 * output_scale,
      .target_height = has_dock_target ? dock_target.height : 4 * output_scale,
      .target_id =
          has_dock_target ? dock_target.id : (uint64_t)(uintptr_t)output,
      .target_generation = has_dock_target ? dock_target.generation : 1,
      .minimizing = minimizing,
      .duration_ms = GENIE_DURATION_MS,
      .finished = genie_finished,
      .data = toplevel,
  };
  bool source_was_enabled = toplevel->scene_tree->node.enabled;
  if (!source_was_enabled) {
    wlr_scene_node_set_enabled(&toplevel->scene_tree->node, true);
  }
  toplevel->animation = tinywl_genie_start(&options);
  if (toplevel->animation == NULL) {
    wlr_scene_node_set_enabled(&toplevel->scene_tree->node, source_was_enabled);
    os_window_cancel_animation(&toplevel->state);
    return false;
  }

  wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);
  if (minimizing) {
    wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, false);
    if (toplevel->server->seat->keyboard_state.focused_surface ==
        toplevel->xdg_toplevel->base->surface) {
      wlr_seat_keyboard_clear_focus(toplevel->server->seat);
    }
  }
  return true;
}

static struct tinywl_toplevel *
latest_minimized_toplevel(struct tinywl_server *server) {
  struct tinywl_toplevel *toplevel;
  wl_list_for_each(toplevel, &server->toplevels, link) {
    if (toplevel->mapped && toplevel->state.state == OS_WINDOW_MINIMIZED &&
        toplevel->animation == NULL) {
      return toplevel;
    }
  }
  return NULL;
}

static struct tinywl_toplevel *
latest_animating_toplevel(struct tinywl_server *server) {
  struct tinywl_toplevel *toplevel;
  wl_list_for_each(toplevel, &server->toplevels, link) {
    if (toplevel->mapped && toplevel->animation != NULL) {
      return toplevel;
    }
  }
  return NULL;
}

static struct tinywl_toplevel *
active_visible_toplevel(struct tinywl_server *server) {
  struct wlr_surface *focused = server->seat->keyboard_state.focused_surface;
  if (focused != NULL) {
    focused = wlr_surface_get_root_surface(focused);
  }
  struct tinywl_toplevel *toplevel;
  wl_list_for_each(toplevel, &server->toplevels, link) {
    if (!toplevel->mapped || !os_window_is_visible(&toplevel->state) ||
        toplevel->animation != NULL) {
      continue;
    }
    if (focused == toplevel->xdg_toplevel->base->surface) {
      return toplevel;
    }
  }
  wl_list_for_each(toplevel, &server->toplevels, link) {
    if (toplevel->mapped && os_window_is_visible(&toplevel->state) &&
        toplevel->animation == NULL) {
      return toplevel;
    }
  }
  return NULL;
}

static void keyboard_handle_modifiers(struct wl_listener *listener,
                                      void *data) {
  /* This event is raised when a modifier key, such as shift or alt, is
   * pressed. We simply communicate this to the client. */
  struct tinywl_keyboard *keyboard =
      wl_container_of(listener, keyboard, modifiers);
  (void)data;
  /*
   * A seat can only have one keyboard, but this is a limitation of the
   * Wayland protocol - not wlroots. We assign all connected keyboards to the
   * same seat. You can swap out the underlying wlr_keyboard like this and
   * wlr_seat handles this transparently.
   */
  wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
  /* Send modifiers to the client. */
  wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
                                     &keyboard->wlr_keyboard->modifiers);
}

static bool handle_keybinding(struct tinywl_server *server, xkb_keysym_t sym) {
  /*
   * Here we handle compositor keybindings. This is when the compositor is
   * processing keys, rather than passing them on to the client for its own
   * processing.
   *
   * This function assumes Alt is held down.
   */
  switch (sym) {
  case XKB_KEY_Escape:
    wlr_log(WLR_ERROR, "Alt+Escape requested compositor termination");
    wl_display_terminate(server->wl_display);
    break;
  case XKB_KEY_F1: {
    /* Cycle to the next toplevel */
    struct tinywl_toplevel *next_toplevel;
    wl_list_for_each_reverse(next_toplevel, &server->toplevels, link) {
      if (next_toplevel->mapped &&
          os_window_is_visible(&next_toplevel->state) &&
          next_toplevel->animation == NULL) {
        focus_toplevel(next_toplevel,
                       next_toplevel->xdg_toplevel->base->surface);
        break;
      }
    }
    break;
  }
  default:
    return false;
  }
  return true;
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
  /* This event is raised when a key is pressed or released. */
  struct tinywl_keyboard *keyboard = wl_container_of(listener, keyboard, key);
  struct tinywl_server *server = keyboard->server;
  struct wlr_keyboard_key_event *event = data;
  struct wlr_seat *seat = server->seat;

  /* Translate libinput keycode -> xkbcommon */
  uint32_t keycode = event->keycode + 8;
  /* Get a list of keysyms based on the keymap for this keyboard */
  const xkb_keysym_t *syms;
  int nsyms =
      xkb_state_key_get_syms(keyboard->wlr_keyboard->xkb_state, keycode, &syms);

  bool handled = false;
  uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
  if ((modifiers & WLR_MODIFIER_ALT) &&
      event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
    /* If alt is held down and this button was _pressed_, we attempt to
     * process it as a compositor keybinding. */
    for (int i = 0; i < nsyms; i++) {
      handled = handle_keybinding(server, syms[i]);
    }
  }

  if (!handled) {
    /* Otherwise, we pass it along to the client. */
    wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
    wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode,
                                 event->state);
  }
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
  /* This event is raised by the keyboard base wlr_input_device to signal
   * the destruction of the wlr_keyboard. It will no longer receive events
   * and should be destroyed.
   */
  struct tinywl_keyboard *keyboard =
      wl_container_of(listener, keyboard, destroy);
  (void)data;
  wl_list_remove(&keyboard->modifiers.link);
  wl_list_remove(&keyboard->key.link);
  wl_list_remove(&keyboard->destroy.link);
  wl_list_remove(&keyboard->link);
  os_lifetime_release(&keyboard->server->lifetime, OS_LIVE_KEYBOARD);
  free(keyboard);
}

static void server_new_keyboard(struct tinywl_server *server,
                                struct wlr_input_device *device) {
  struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

  struct tinywl_keyboard *keyboard = calloc(1, sizeof(*keyboard));
  if (keyboard == NULL) {
    wlr_log(WLR_ERROR, "keyboard.create allocation failed");
    return;
  }
  keyboard->server = server;
  keyboard->wlr_keyboard = wlr_keyboard;

  /* We need to prepare an XKB keymap and assign it to the keyboard. This
   * assumes the defaults (e.g. layout = "us"). */
  struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  struct xkb_keymap *keymap =
      xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);

  wlr_keyboard_set_keymap(wlr_keyboard, keymap);
  xkb_keymap_unref(keymap);
  xkb_context_unref(context);
  wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

  /* Here we set up listeners for keyboard events. */
  keyboard->modifiers.notify = keyboard_handle_modifiers;
  wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
  keyboard->key.notify = keyboard_handle_key;
  wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
  keyboard->destroy.notify = keyboard_handle_destroy;
  wl_signal_add(&device->events.destroy, &keyboard->destroy);

  wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);

  /* And add the keyboard to our list of keyboards */
  wl_list_insert(&server->keyboards, &keyboard->link);
  os_lifetime_acquire(&server->lifetime, OS_LIVE_KEYBOARD);
}

static void server_new_pointer(struct tinywl_server *server,
                               struct wlr_input_device *device) {
  if (wlr_input_device_is_libinput(device)) {
    struct libinput_device *libinput_device =
        wlr_libinput_get_device_handle(device);
    if (libinput_device_config_accel_is_available(libinput_device)) {
      double speed = TINYWL_POINTER_ACCEL_DEFAULT;
      const char *setting = getenv("TINYWL_POINTER_ACCEL");
      if (setting != NULL) {
        char *end = NULL;
        errno = 0;
        double configured_speed = strtod(setting, &end);
        if (errno == 0 && end != setting && *end == '\0' &&
            configured_speed >= -1.0 && configured_speed <= 1.0) {
          speed = configured_speed;
        } else {
          wlr_log(WLR_ERROR,
                  "invalid TINYWL_POINTER_ACCEL='%s'; using default %.2f",
                  setting, speed);
        }
      }

      enum libinput_config_status status =
          libinput_device_config_accel_set_speed(libinput_device, speed);
      if (status == LIBINPUT_CONFIG_STATUS_SUCCESS) {
        wlr_log(WLR_INFO, "pointer acceleration speed set to %.2f", speed);
      } else {
        wlr_log(WLR_ERROR, "failed to set pointer acceleration speed: %d",
                status);
      }
    }
  }

  wlr_cursor_attach_input_device(server->cursor, device);
}

static void server_new_input(struct wl_listener *listener, void *data) {
  /* This event is raised by the backend when a new input device becomes
   * available. */
  struct tinywl_server *server = wl_container_of(listener, server, new_input);
  struct wlr_input_device *device = data;
  switch (device->type) {
  case WLR_INPUT_DEVICE_KEYBOARD:
    server_new_keyboard(server, device);
    break;
  case WLR_INPUT_DEVICE_POINTER:
    server_new_pointer(server, device);
    break;
  default:
    break;
  }
  /* We need to let the wlr_seat know what our capabilities are, which is
   * communiciated to the client. In TinyWL we always have a cursor, even if
   * there are no pointer devices, so we always include that capability. */
  uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
  if (!wl_list_empty(&server->keyboards)) {
    caps |= WL_SEAT_CAPABILITY_KEYBOARD;
  }
  wlr_seat_set_capabilities(server->seat, caps);
}

static void seat_request_cursor(struct wl_listener *listener, void *data) {
  struct tinywl_server *server =
      wl_container_of(listener, server, request_cursor);
  /* This event is raised by the seat when a client provides a cursor image */
  struct wlr_seat_pointer_request_set_cursor_event *event = data;
  struct wlr_seat_client *focused_client =
      server->seat->pointer_state.focused_client;
  /* This can be sent by any client, so we check to make sure this one is
   * actually has pointer focus first. */
  if (focused_client == event->seat_client) {
    /* Once we've vetted the client, we can tell the cursor to use the
     * provided surface as the cursor image. It will set the hardware cursor
     * on the output that it's currently on and continue to do so as the
     * cursor moves between outputs. */
    wlr_cursor_set_surface(server->cursor, event->surface, event->hotspot_x,
                           event->hotspot_y);
  }
}

static void seat_request_set_selection(struct wl_listener *listener,
                                       void *data) {
  /* This event is raised by the seat when a client wants to set the selection,
   * usually when the user copies something. wlroots allows compositors to
   * ignore such requests if they so choose, but in tinywl we always honor
   */
  struct tinywl_server *server =
      wl_container_of(listener, server, request_set_selection);
  struct wlr_seat_request_set_selection_event *event = data;
  wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static struct tinywl_toplevel *desktop_toplevel_at(struct tinywl_server *server,
                                                   double lx, double ly,
                                                   struct wlr_surface **surface,
                                                   double *sx, double *sy) {
  *surface = NULL;
  struct wlr_scene_node *node =
      wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
  if (node == NULL) {
    return NULL;
  }
  if (node->type == WLR_SCENE_NODE_BUFFER) {
    struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface *scene_surface =
        wlr_scene_surface_try_from_buffer(scene_buffer);
    if (scene_surface != NULL) {
      *surface = scene_surface->surface;
    }
  }

  struct wlr_scene_tree *tree = node->parent;
  while (tree != NULL && tree->node.data == NULL) {
    tree = tree->node.parent;
  }
  return tree != NULL ? tree->node.data : NULL;
}

static void reset_cursor_mode(struct tinywl_server *server) {
  /* Reset the cursor mode to passthrough. */
  server->cursor_mode = TINYWL_CURSOR_PASSTHROUGH;
  server->grabbed_toplevel = NULL;
  /* An interactive move/resize set a non-default cursor; restore the
   * background arrow now that the gesture is over. (process_cursor_motion
   * early-returns during MOVE/RESIZE, so it cannot do this for us.) */
  os_cursor_apply(server->cursor, server->cursor_mgr, "left_ptr");
}

static void process_cursor_move(struct tinywl_server *server, uint32_t time) {
  (void)time;
  /* Move the grabbed toplevel to the new position. */
  struct tinywl_toplevel *toplevel = server->grabbed_toplevel;
  wlr_scene_node_set_position(&toplevel->scene_tree->node,
                              server->cursor->x - server->grab_x,
                              server->cursor->y - server->grab_y);
}

static void process_cursor_resize(struct tinywl_server *server, uint32_t time) {
  (void)time;
  /*
   * Resizing the grabbed toplevel can be a little bit complicated, because we
   * could be resizing from any corner or edge. This not only resizes the
   * toplevel on one or two axes, but can also move the toplevel if you resize
   * from the top or left edges (or top-left corner).
   *
   * Note that some shortcuts are taken here. In a more fleshed-out
   * compositor, you'd wait for the client to prepare a buffer at the new
   * size, then commit any movement that was prepared.
   */
  struct tinywl_toplevel *toplevel = server->grabbed_toplevel;
  double border_x = server->cursor->x - server->grab_x;
  double border_y = server->cursor->y - server->grab_y;
  int new_left = server->grab_geobox.x;
  int new_right = server->grab_geobox.x + server->grab_geobox.width;
  int new_top = server->grab_geobox.y;
  int new_bottom = server->grab_geobox.y + server->grab_geobox.height;

  if (server->resize_edges & WLR_EDGE_TOP) {
    new_top = border_y;
    if (new_top >= new_bottom) {
      new_top = new_bottom - 1;
    }
  } else if (server->resize_edges & WLR_EDGE_BOTTOM) {
    new_bottom = border_y;
    if (new_bottom <= new_top) {
      new_bottom = new_top + 1;
    }
  }
  if (server->resize_edges & WLR_EDGE_LEFT) {
    new_left = border_x;
    if (new_left >= new_right) {
      new_left = new_right - 1;
    }
  } else if (server->resize_edges & WLR_EDGE_RIGHT) {
    new_right = border_x;
    if (new_right <= new_left) {
      new_right = new_left + 1;
    }
  }

  struct wlr_box geo_box = toplevel->xdg_toplevel->base->geometry;
  wlr_scene_node_set_position(&toplevel->scene_tree->node, new_left - geo_box.x,
                              new_top - geo_box.y);

  int new_width = new_right - new_left;
  int new_height = new_bottom - new_top;
  wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, new_width, new_height);
}

static void process_cursor_motion(struct tinywl_server *server, uint32_t time) {
  /* If the mode is non-passthrough, delegate to those functions. */
  if (server->cursor_mode == TINYWL_CURSOR_MOVE) {
    process_cursor_move(server, time);
    return;
  } else if (server->cursor_mode == TINYWL_CURSOR_RESIZE) {
    process_cursor_resize(server, time);
    return;
  }

  /* Otherwise, find the toplevel under the pointer and send the event along. */
  double sx, sy;
  struct wlr_seat *seat = server->seat;
  struct wlr_surface *surface = NULL;
  struct tinywl_toplevel *toplevel = desktop_toplevel_at(
      server, server->cursor->x, server->cursor->y, &surface, &sx, &sy);
  if (!toplevel) {
    /* If there's no toplevel under the cursor, set the cursor image to a
     * default. This is what makes the cursor image appear when you move it
     * around the screen, not over any toplevels. */
    os_cursor_apply(server->cursor, server->cursor_mgr, "left_ptr");
  }
  if (surface) {
    /*
     * Send pointer enter and motion events.
     *
     * The enter event gives the surface "pointer focus", which is distinct
     * from keyboard focus. You get pointer focus by moving the pointer over
     * a window.
     *
     * Note that wlroots will avoid sending duplicate enter/motion events if
     * the surface has already has pointer focus or if the client is already
     * aware of the coordinates passed.
     */
    wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
    wlr_seat_pointer_notify_motion(seat, time, sx, sy);
  } else {
    /* Clear pointer focus so future button events and such are not sent to
     * the last client to have the cursor over it. */
    wlr_seat_pointer_clear_focus(seat);
  }
}

static void server_cursor_motion(struct wl_listener *listener, void *data) {
  /* This event is forwarded by the cursor when a pointer emits a _relative_
   * pointer motion event (i.e. a delta) */
  struct tinywl_server *server =
      wl_container_of(listener, server, cursor_motion);
  struct wlr_pointer_motion_event *event = data;
  /* The cursor doesn't move unless we tell it to. The cursor automatically
   * handles constraining the motion to the output layout, as well as any
   * special configuration applied for the specific input device which
   * generated the event. You can pass NULL for the device if you want to move
   * the cursor around without any input. */
  wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x,
                  event->delta_y);
  process_cursor_motion(server, event->time_msec);
}

static void server_cursor_motion_absolute(struct wl_listener *listener,
                                          void *data) {
  /* This event is forwarded by the cursor when a pointer emits an _absolute_
   * motion event, from 0..1 on each axis. This happens, for example, when
   * wlroots is running under a Wayland window rather than KMS+DRM, and you
   * move the mouse over the window. You could enter the window from any edge,
   * so we have to warp the mouse there. There is also some hardware which
   * emits these events. */
  struct tinywl_server *server =
      wl_container_of(listener, server, cursor_motion_absolute);
  struct wlr_pointer_motion_absolute_event *event = data;
  wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x,
                           event->y);
  process_cursor_motion(server, event->time_msec);
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
  /* This event is forwarded by the cursor when a pointer emits a button
   * event. */
  struct tinywl_server *server =
      wl_container_of(listener, server, cursor_button);
  struct wlr_pointer_button_event *event = data;
  double sx, sy;
  struct wlr_surface *surface = NULL;
  struct tinywl_toplevel *toplevel = desktop_toplevel_at(
      server, server->cursor->x, server->cursor->y, &surface, &sx, &sy);
  if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
    if (server->consume_dock_click) {
      server->consume_dock_click = false;
      return;
    }
    if (server->seat->pointer_state.focused_surface != NULL) {
      wlr_seat_pointer_notify_button(server->seat, event->time_msec,
                                     event->button, event->state);
    }
    reset_cursor_mode(server);
    return;
  }

  if (event->button == BTN_LEFT && surface != NULL) {
    struct wlr_surface *root = wlr_surface_get_root_surface(surface);
    struct wlr_layer_surface_v1 *layer_surface =
        wlr_layer_surface_v1_try_from_wlr_surface(root);
    if (layer_surface != NULL && layer_surface->namespace != NULL &&
        strcmp(layer_surface->namespace, "desktop-dock") == 0) {
      struct tinywl_toplevel *animating = latest_animating_toplevel(server);
      if (animating != NULL) {
        if (os_window_reverse_animation(&animating->state) ==
            OS_WINDOW_TRANSITION_OK)
          tinywl_genie_reverse(animating->animation);
        server->consume_dock_click = true;
        return;
      }
      struct tinywl_toplevel *minimized = latest_minimized_toplevel(server);
      if (minimized != NULL &&
          start_genie_animation(minimized, layer_surface->output, false)) {
        server->consume_dock_click = true;
        return;
      }
      struct tinywl_toplevel *visible = active_visible_toplevel(server);
      if (visible != NULL &&
          start_genie_animation(visible, layer_surface->output, true)) {
        server->consume_dock_click = true;
        return;
      }
    }
  }

  if (toplevel != NULL) {
    focus_toplevel(toplevel, toplevel->xdg_toplevel->base->surface);
  }
  if (surface != NULL) {
    wlr_seat_pointer_notify_button(server->seat, event->time_msec,
                                   event->button, event->state);
    return;
  }
  if (toplevel == NULL || event->button != BTN_LEFT) {
    return;
  }

  int tx, ty;
  wlr_scene_node_coords(&toplevel->scene_tree->node, &tx, &ty);
  double rx = server->cursor->x - tx;
  double ry = server->cursor->y - ty;
  if (ry < 0 && ry >= -SSD_TITLE_HEIGHT) {
    int button = ssd_button_at(rx, ry);
    if (button == SSD_ICON_CLOSE) {
      wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
      return;
    }
    if (button == SSD_ICON_MAXIMIZE) {
      struct wlr_output *output = wlr_output_layout_output_at(
          server->output_layout, server->cursor->x, server->cursor->y);
      set_toplevel_maximized(toplevel, output, !toplevel->maximized);
      return;
    }
    if (button == SSD_ICON_MINIMIZE) {
      struct wlr_output *output = wlr_output_layout_output_at(
          server->output_layout, server->cursor->x, server->cursor->y);
      start_genie_animation(toplevel, output, true);
      return;
    }
    begin_interactive(toplevel, TINYWL_CURSOR_MOVE, 0);
    return;
  }

  uint32_t edges = 0;
  if (rx < 0)
    edges |= WLR_EDGE_LEFT;
  if (rx >= toplevel->content_width)
    edges |= WLR_EDGE_RIGHT;
  if (ry >= toplevel->content_height)
    edges |= WLR_EDGE_BOTTOM;
  if (edges != 0) {
    begin_interactive(toplevel, TINYWL_CURSOR_RESIZE, edges);
  }
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
  /* This event is forwarded by the cursor when a pointer emits an axis event,
   * for example when you move the scroll wheel. */
  struct tinywl_server *server = wl_container_of(listener, server, cursor_axis);
  struct wlr_pointer_axis_event *event = data;
  /* Notify the client with pointer focus of the axis event. */
  wlr_seat_pointer_notify_axis(
      server->seat, event->time_msec, event->orientation, event->delta,
      event->delta_discrete, event->source, event->relative_direction);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
  /* This event is forwarded by the cursor when a pointer emits an frame
   * event. Frame events are sent after regular pointer events to group
   * multiple events together. For instance, two axis events may happen at the
   * same time, in which case a frame event won't be sent in between. */
  struct tinywl_server *server =
      wl_container_of(listener, server, cursor_frame);
  (void)data;
  /* Notify the client with pointer focus of the frame event. */
  wlr_seat_pointer_notify_frame(server->seat);
}

static void output_frame(struct wl_listener *listener, void *data) {
  /* This function is called every time an output is ready to display a frame,
   * generally at the output's refresh rate (e.g. 60Hz). */
  struct tinywl_output *output = wl_container_of(listener, output, frame);
  (void)data;
  struct wlr_scene *scene = output->server->scene;

  struct wlr_scene_output *scene_output =
      wlr_scene_get_scene_output(scene, output->wlr_output);

  /* Render the scene if needed and commit the output */
  set_output_dock_enabled(output, false);
  os_core_commit_scene_frame(scene_output, output->server->renderer,
                             render_system_overlays, output);
  set_output_dock_enabled(output, true);

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_request_state(struct wl_listener *listener, void *data) {
  /* This function is called when the backend requests a new state for
   * the output. For example, Wayland and X11 backends request a new mode
   * when the output window is resized. */
  struct tinywl_output *output =
      wl_container_of(listener, output, request_state);
  const struct wlr_output_event_request_state *event = data;
  wlr_output_commit_state(output->wlr_output, event->state);
}

static struct wlr_scene_tree *layer_tree_for(struct tinywl_server *server,
                                             uint32_t layer) {
  switch (layer) {
  case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
    return server->layer_background;
  case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
    return server->layer_bottom;
  case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
  case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
    return server->layer_overlay;
  default:
    return server->layer_background;
  }
}

static void configure_layer_surface(struct tinywl_layer_surface *surface) {
  struct wlr_output *output = surface->layer_surface->output;
  if (output == NULL) {
    output =
        wlr_output_layout_get_center_output(surface->server->output_layout);
    surface->layer_surface->output = output;
  }
  if (output == NULL) {
    return;
  }
  struct wlr_box full;
  wlr_output_layout_get_box(surface->server->output_layout, output, &full);
  struct wlr_box usable = full;
  wlr_scene_layer_surface_v1_configure(surface->scene_layer, &full, &usable);
}

static void cancel_animations_for_target(struct tinywl_layer_surface *surface) {
  uint64_t id = (uint64_t)(uintptr_t)surface;
  struct tinywl_toplevel *toplevel;
  wl_list_for_each(toplevel, &surface->server->toplevels, link) {
    if (!tinywl_genie_targets(toplevel->animation, id,
                              surface->target_generation))
      continue;
    tinywl_genie_cancel(toplevel->animation);
    toplevel->animation = NULL;
    os_window_cancel_animation(&toplevel->state);
    wlr_scene_node_set_enabled(&toplevel->scene_tree->node,
                               os_window_is_visible(&toplevel->state));
  }
}

static void layer_surface_commit(struct wl_listener *listener, void *data) {
  struct tinywl_layer_surface *surface =
      wl_container_of(listener, surface, commit);
  (void)data;
  configure_layer_surface(surface);
}

static void layer_surface_destroy(struct wl_listener *listener, void *data) {
  struct tinywl_layer_surface *surface =
      wl_container_of(listener, surface, destroy);
  (void)data;
  cancel_animations_for_target(surface);
  wl_list_remove(&surface->commit.link);
  wl_list_remove(&surface->destroy.link);
  wl_list_remove(&surface->link);
  os_lifetime_release(&surface->server->lifetime, OS_LIVE_LAYER_SURFACE);
  free(surface);
}

static void server_new_layer_surface(struct wl_listener *listener, void *data) {
  struct tinywl_server *server =
      wl_container_of(listener, server, new_layer_surface);
  struct wlr_layer_surface_v1 *layer_surface = data;
  if (layer_surface->output == NULL) {
    layer_surface->output =
        wlr_output_layout_get_center_output(server->output_layout);
  }

  struct tinywl_layer_surface *surface = calloc(1, sizeof(*surface));
  if (surface == NULL) {
    wlr_log(WLR_ERROR, "layer_surface.create allocation failed");
    wlr_layer_surface_v1_destroy(layer_surface);
    return;
  }
  surface->server = server;
  surface->target_generation = 1;
  surface->layer_surface = layer_surface;
  surface->scene_layer = wlr_scene_layer_surface_v1_create(
      layer_tree_for(server, layer_surface->current.layer), layer_surface);
  layer_surface->data = surface;
  wl_list_insert(&server->layer_surfaces, &surface->link);

  surface->commit.notify = layer_surface_commit;
  wl_signal_add(&layer_surface->surface->events.commit, &surface->commit);
  surface->destroy.notify = layer_surface_destroy;
  wl_signal_add(&layer_surface->events.destroy, &surface->destroy);
  os_lifetime_acquire(&server->lifetime, OS_LIVE_LAYER_SURFACE);
}

static void output_destroy(struct wl_listener *listener, void *data) {
  struct tinywl_output *output = wl_container_of(listener, output, destroy);
  (void)data;

  struct tinywl_toplevel *toplevel;
  wl_list_for_each(toplevel, &output->server->toplevels, link) {
    if (!tinywl_genie_uses_output(toplevel->animation, output->wlr_output))
      continue;
    tinywl_genie_cancel(toplevel->animation);
    toplevel->animation = NULL;
    os_window_cancel_animation(&toplevel->state);
    wlr_scene_node_set_enabled(&toplevel->scene_tree->node,
                               os_window_is_visible(&toplevel->state));
  }

  wl_list_remove(&output->frame.link);
  wl_list_remove(&output->request_state.link);
  wl_list_remove(&output->destroy.link);
  wl_list_remove(&output->link);
  os_lifetime_release(&output->server->lifetime, OS_LIVE_OUTPUT);
  free(output);
}

static void server_new_output(struct wl_listener *listener, void *data) {
  /* This event is raised by the backend when a new output (aka a display or
   * monitor) becomes available. */
  struct tinywl_server *server = wl_container_of(listener, server, new_output);
  struct wlr_output *wlr_output = data;

  /* Configures the output created by the backend to use our allocator
   * and our renderer. Must be done once, before commiting the output */
  wlr_output_init_render(wlr_output, server->allocator, server->renderer);

  /* The output may be disabled, switch it on. */
  struct wlr_output_state state;
  wlr_output_state_init(&state);
  wlr_output_state_set_enabled(&state, true);

  /* Some backends don't have modes. DRM+KMS does, and we need to set a mode
   * before we can use the output. The mode is a tuple of (width, height,
   * refresh rate), and each monitor supports only a specific set of modes. We
   * just pick the monitor's preferred mode, a more sophisticated compositor
   * would let the user configure it. */
  struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
  if (mode != NULL) {
    wlr_output_state_set_mode(&state, mode);
  }

  /* Atomically applies the new output state. */
  wlr_output_commit_state(wlr_output, &state);
  wlr_output_state_finish(&state);

  /* Allocates and configures our state for this output */
  struct tinywl_output *output = calloc(1, sizeof(*output));
  if (output == NULL) {
    wlr_log(WLR_ERROR, "output.create allocation failed");
    return;
  }
  output->wlr_output = wlr_output;
  output->server = server;

  /* Sets up a listener for the frame event. */
  output->frame.notify = output_frame;
  wl_signal_add(&wlr_output->events.frame, &output->frame);

  /* Sets up a listener for the state request event. */
  output->request_state.notify = output_request_state;
  wl_signal_add(&wlr_output->events.request_state, &output->request_state);

  /* Sets up a listener for the destroy event. */
  output->destroy.notify = output_destroy;
  wl_signal_add(&wlr_output->events.destroy, &output->destroy);

  wl_list_insert(&server->outputs, &output->link);
  os_lifetime_acquire(&server->lifetime, OS_LIVE_OUTPUT);

  /* Adds this to the output layout. The add_auto function arranges outputs
   * from left-to-right in the order they appear. A more sophisticated
   * compositor would let the user configure the arrangement of outputs in the
   * layout.
   *
   * The output layout utility automatically adds a wl_output global to the
   * display, which Wayland clients can see to find out information about the
   * output (such as DPI, scale factor, manufacturer, etc).
   */
  struct wlr_output_layout_output *l_output =
      wlr_output_layout_add_auto(server->output_layout, wlr_output);
  struct wlr_scene_output *scene_output =
      wlr_scene_output_create(server->scene, wlr_output);
  wlr_scene_output_layout_add_output(server->scene_layout, l_output,
                                     scene_output);
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
  /* Called when the surface is mapped, or ready to display on-screen. */
  struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, map);
  (void)data;

  toplevel->mapped = true;
  os_window_state_init(&toplevel->state);
  wlr_scene_node_set_enabled(&toplevel->scene_tree->node, true);
  wl_list_insert(&toplevel->server->toplevels, &toplevel->link);
  update_ssd(toplevel);
  if (toplevel->scene_tree->node.x == 0 && toplevel->scene_tree->node.y == 0) {
    struct wlr_output *output =
        wlr_output_layout_get_center_output(toplevel->server->output_layout);
    if (output != NULL) {
      struct wlr_box box;
      struct wlr_box geo = toplevel->xdg_toplevel->base->geometry;
      wlr_output_layout_get_box(toplevel->server->output_layout, output, &box);
      int x = box.x + (box.width - geo.width) / 2;
      int y =
          box.y +
          (box.height - geo.height - SSD_TITLE_HEIGHT - SSD_BORDER_WIDTH) / 2 +
          SSD_TITLE_HEIGHT;
      wlr_scene_node_set_position(&toplevel->scene_tree->node, x, y);
    }
  }

  focus_toplevel(toplevel, toplevel->xdg_toplevel->base->surface);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
  /* Called when the surface is unmapped, and should no longer be shown. */
  struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
  (void)data;

  /* Reset the cursor mode if the grabbed toplevel was unmapped. */
  if (toplevel == toplevel->server->grabbed_toplevel) {
    reset_cursor_mode(toplevel->server);
  }

  if (toplevel->animation != NULL) {
    tinywl_genie_cancel(toplevel->animation);
    toplevel->animation = NULL;
    os_window_cancel_animation(&toplevel->state);
  }
  toplevel->mapped = false;
  os_window_state_init(&toplevel->state);

  wlr_scene_node_set_enabled(&toplevel->decoration_tree->node, false);
  wl_list_remove(&toplevel->link);
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
  /* Called when a new surface state is committed. */
  struct tinywl_toplevel *toplevel =
      wl_container_of(listener, toplevel, commit);
  (void)data;

  if (toplevel->xdg_toplevel->base->initial_commit) {
    /* When an xdg_surface performs an initial commit, the compositor must
     * reply with a configure so the client can map the surface. tinywl
     * configures the xdg_toplevel with 0,0 size to let the client pick the
     * dimensions itself. */
    if (toplevel->xdg_toplevel->requested.maximized) {
      struct wlr_output *output =
          wlr_output_layout_get_center_output(toplevel->server->output_layout);
      set_toplevel_maximized(toplevel, output, true);
    } else {
      wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
    }
  }
  update_ssd(toplevel);
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
  /* Called when the xdg_toplevel is destroyed. */
  struct tinywl_toplevel *toplevel =
      wl_container_of(listener, toplevel, destroy);
  (void)data;

  wl_list_remove(&toplevel->map.link);
  wl_list_remove(&toplevel->unmap.link);
  wl_list_remove(&toplevel->commit.link);
  wl_list_remove(&toplevel->destroy.link);
  wl_list_remove(&toplevel->request_move.link);
  wl_list_remove(&toplevel->request_resize.link);
  wl_list_remove(&toplevel->request_maximize.link);
  wl_list_remove(&toplevel->request_minimize.link);
  wl_list_remove(&toplevel->request_fullscreen.link);

  os_window_begin_destroy(&toplevel->state);
  if (toplevel->animation != NULL) {
    tinywl_genie_cancel(toplevel->animation);
  }

  os_lifetime_release(&toplevel->server->lifetime, OS_LIVE_WINDOW);
  free(toplevel);
}

static void begin_interactive(struct tinywl_toplevel *toplevel,
                              enum tinywl_cursor_mode mode, uint32_t edges) {
  /* This function sets up an interactive move or resize operation, where the
   * compositor stops propegating pointer events to clients and instead
   * consumes them itself, to move or resize windows. */
  struct tinywl_server *server = toplevel->server;
  struct wlr_surface *focused_surface =
      server->seat->pointer_state.focused_surface;
  if (focused_surface != NULL &&
      toplevel->xdg_toplevel->base->surface !=
          wlr_surface_get_root_surface(focused_surface)) {
    /* Deny move/resize requests from unfocused clients. */
    return;
  }
  server->grabbed_toplevel = toplevel;
  server->cursor_mode = mode;

  /* Set the gesture cursor on press. process_cursor_motion early-returns
   * during MOVE/RESIZE, so the cursor chosen here stays until the button is
   * released, when reset_cursor_mode restores the background arrow. Corner
   * resizes only have h/v assets; route them to the horizontal arrow. */
  if (mode == TINYWL_CURSOR_MOVE) {
    os_cursor_apply(server->cursor, server->cursor_mgr, "move");
  } else {
    os_cursor_apply(server->cursor, server->cursor_mgr,
                    (edges & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT))
                        ? "sb_h_double_arrow"
                        : "sb_v_double_arrow");
  }

  if (mode == TINYWL_CURSOR_MOVE) {
    server->grab_x = server->cursor->x - toplevel->scene_tree->node.x;
    server->grab_y = server->cursor->y - toplevel->scene_tree->node.y;
  } else {
    struct wlr_box geo_box = toplevel->xdg_toplevel->base->geometry;

    double border_x = (toplevel->scene_tree->node.x + geo_box.x) +
                      ((edges & WLR_EDGE_RIGHT) ? geo_box.width : 0);
    double border_y = (toplevel->scene_tree->node.y + geo_box.y) +
                      ((edges & WLR_EDGE_BOTTOM) ? geo_box.height : 0);
    server->grab_x = server->cursor->x - border_x;
    server->grab_y = server->cursor->y - border_y;

    server->grab_geobox = geo_box;
    server->grab_geobox.x += toplevel->scene_tree->node.x;
    server->grab_geobox.y += toplevel->scene_tree->node.y;

    server->resize_edges = edges;
  }
}

static void xdg_toplevel_request_move(struct wl_listener *listener,
                                      void *data) {
  /* This event is raised when a client would like to begin an interactive
   * move, typically because the user clicked on their client-side
   * decorations. Note that a more sophisticated compositor should check the
   * provided serial against a list of button press serials sent to this
   * client, to prevent the client from requesting this whenever they want. */
  struct tinywl_toplevel *toplevel =
      wl_container_of(listener, toplevel, request_move);
  (void)data;
  begin_interactive(toplevel, TINYWL_CURSOR_MOVE, 0);
}

static void xdg_toplevel_request_resize(struct wl_listener *listener,
                                        void *data) {
  /* This event is raised when a client would like to begin an interactive
   * resize, typically because the user clicked on their client-side
   * decorations. Note that a more sophisticated compositor should check the
   * provided serial against a list of button press serials sent to this
   * client, to prevent the client from requesting this whenever they want. */
  struct wlr_xdg_toplevel_resize_event *event = data;
  struct tinywl_toplevel *toplevel =
      wl_container_of(listener, toplevel, request_resize);
  (void)data;
  begin_interactive(toplevel, TINYWL_CURSOR_RESIZE, event->edges);
}

static void xdg_toplevel_request_maximize(struct wl_listener *listener,
                                          void *data) {
  struct tinywl_toplevel *toplevel =
      wl_container_of(listener, toplevel, request_maximize);
  (void)data;
  if (toplevel->xdg_toplevel->base->initialized) {
    struct wlr_output *output =
        wlr_output_layout_get_center_output(toplevel->server->output_layout);
    set_toplevel_maximized(toplevel, output,
                           toplevel->xdg_toplevel->requested.maximized);
  }
}

static void xdg_toplevel_request_minimize(struct wl_listener *listener,
                                          void *data) {
  struct tinywl_toplevel *toplevel =
      wl_container_of(listener, toplevel, request_minimize);
  (void)data;
  struct wlr_output *output = wlr_output_layout_output_at(
      toplevel->server->output_layout, toplevel->scene_tree->node.x,
      toplevel->scene_tree->node.y);
  start_genie_animation(toplevel, output, true);
}

static void xdg_toplevel_request_fullscreen(struct wl_listener *listener,
                                            void *data) {
  /* Just as with request_maximize, we must send a configure here. */
  struct tinywl_toplevel *toplevel =
      wl_container_of(listener, toplevel, request_fullscreen);
  (void)data;
  if (toplevel->xdg_toplevel->base->initialized) {
    wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
  }
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
  /* This event is raised when a client creates a new toplevel (application
   * window). */
  struct tinywl_server *server =
      wl_container_of(listener, server, new_xdg_toplevel);
  struct wlr_xdg_toplevel *xdg_toplevel = data;

  /* Allocate a tinywl_toplevel for this surface */
  struct tinywl_toplevel *toplevel = calloc(1, sizeof(*toplevel));
  if (toplevel == NULL) {
    wlr_log(WLR_ERROR, "failed to allocate toplevel");
    return;
  }
  toplevel->server = server;
  os_window_state_init(&toplevel->state);
  os_lifetime_acquire(&server->lifetime, OS_LIVE_WINDOW);
  toplevel->xdg_toplevel = xdg_toplevel;
  toplevel->scene_tree = wlr_scene_tree_create(server->layer_toplevel);
  toplevel->scene_tree->node.data = toplevel;
  toplevel->content_tree =
      wlr_scene_xdg_surface_create(toplevel->scene_tree, xdg_toplevel->base);
  xdg_toplevel->base->data = toplevel->content_tree;
  toplevel->decoration_tree = wlr_scene_tree_create(toplevel->scene_tree);
  wlr_scene_node_set_enabled(&toplevel->decoration_tree->node, false);

  const float title_color[4] = {0.20f, 0.20f, 0.23f, 1.0f};
  const float border_color[4] = {0.08f, 0.08f, 0.09f, 1.0f};
  const float close_color[4] = {1.0f, 0.37f, 0.34f, 1.0f};
  const float minimize_color[4] = {1.0f, 0.74f, 0.18f, 1.0f};
  const float maximize_color[4] = {0.16f, 0.78f, 0.25f, 1.0f};
  toplevel->titlebar = wlr_scene_rect_create(toplevel->decoration_tree, 1,
                                             SSD_TITLE_HEIGHT, title_color);
  toplevel->border_left = wlr_scene_rect_create(
      toplevel->decoration_tree, SSD_BORDER_WIDTH, 1, border_color);
  toplevel->border_right = wlr_scene_rect_create(
      toplevel->decoration_tree, SSD_BORDER_WIDTH, 1, border_color);
  toplevel->border_bottom = wlr_scene_rect_create(
      toplevel->decoration_tree, 1, SSD_BORDER_WIDTH, border_color);
  toplevel->button_close =
      create_ssd_button(toplevel->decoration_tree, close_color, SSD_ICON_CLOSE);
  toplevel->button_minimize = create_ssd_button(
      toplevel->decoration_tree, minimize_color, SSD_ICON_MINIMIZE);
  toplevel->button_maximize = create_ssd_button(
      toplevel->decoration_tree, maximize_color, SSD_ICON_MAXIMIZE);

  /* Listen to the various events it can emit */
  toplevel->map.notify = xdg_toplevel_map;
  wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
  toplevel->unmap.notify = xdg_toplevel_unmap;
  wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
  toplevel->commit.notify = xdg_toplevel_commit;
  wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);

  toplevel->destroy.notify = xdg_toplevel_destroy;
  wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);

  /* cotd */
  toplevel->request_move.notify = xdg_toplevel_request_move;
  wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);
  toplevel->request_resize.notify = xdg_toplevel_request_resize;
  wl_signal_add(&xdg_toplevel->events.request_resize,
                &toplevel->request_resize);
  toplevel->request_maximize.notify = xdg_toplevel_request_maximize;
  wl_signal_add(&xdg_toplevel->events.request_maximize,
                &toplevel->request_maximize);
  toplevel->request_minimize.notify = xdg_toplevel_request_minimize;
  wl_signal_add(&xdg_toplevel->events.request_minimize,
                &toplevel->request_minimize);
  toplevel->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
  wl_signal_add(&xdg_toplevel->events.request_fullscreen,
                &toplevel->request_fullscreen);
}

static void server_new_decoration(struct wl_listener *listener, void *data) {
  struct wlr_xdg_toplevel_decoration_v1 *decoration = data;
  (void)listener;
  if (decoration->toplevel->base->initialized) {
    wlr_xdg_toplevel_decoration_v1_set_mode(
        decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
  } else {
    /* The initial surface commit will schedule the first configure. */
    decoration->scheduled_mode =
        WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
  }
}

static void xdg_popup_commit(struct wl_listener *listener, void *data) {
  /* Called when a new surface state is committed. */
  struct tinywl_popup *popup = wl_container_of(listener, popup, commit);
  (void)data;

  if (popup->xdg_popup->base->initial_commit) {
    /* When an xdg_surface performs an initial commit, the compositor must
     * reply with a configure so the client can map the surface.
     * tinywl sends an empty configure. A more sophisticated compositor
     * might change an xdg_popup's geometry to ensure it's not positioned
     * off-screen, for example. */
    wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
  }
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data) {
  /* Called when the xdg_popup is destroyed. */
  struct tinywl_popup *popup = wl_container_of(listener, popup, destroy);
  (void)data;

  wl_list_remove(&popup->commit.link);
  wl_list_remove(&popup->destroy.link);

  os_lifetime_release(&popup->server->lifetime, OS_LIVE_POPUP);
  free(popup);
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data) {
  /* This event is raised when a client creates a new popup. */
  struct tinywl_server *server =
      wl_container_of(listener, server, new_xdg_popup);
  struct wlr_xdg_popup *xdg_popup = data;

  struct tinywl_popup *popup = calloc(1, sizeof(*popup));
  if (popup == NULL) {
    wlr_log(WLR_ERROR, "popup.create allocation failed");
    wlr_xdg_popup_destroy(xdg_popup);
    return;
  }
  popup->server = server;
  popup->xdg_popup = xdg_popup;
  os_lifetime_acquire(&server->lifetime, OS_LIVE_POPUP);

  /* We must add xdg popups to the scene graph so they get rendered. The
   * wlroots scene graph provides a helper for this, but to use it we must
   * provide the proper parent scene node of the xdg popup. To enable this,
   * we always set the user data field of xdg_surfaces to the corresponding
   * scene node. */
  struct wlr_xdg_surface *parent =
      wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
  assert(parent != NULL);
  struct wlr_scene_tree *parent_tree = parent->data;
  xdg_popup->base->data =
      wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

  popup->commit.notify = xdg_popup_commit;
  wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);

  popup->destroy.notify = xdg_popup_destroy;
  wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

int os_compositor_run(int argc, char *argv[]) {
  wlr_log_init(WLR_INFO, NULL);
  char *startup_cmd = NULL;

  int c;
  while ((c = getopt(argc, argv, "s:h")) != -1) {
    switch (c) {
    case 's':
      startup_cmd = optarg;
      break;
    default:
      printf("Usage: %s [-s startup command]\n", argv[0]);
      return 0;
    }
  }
  if (optind < argc) {
    printf("Usage: %s [-s startup command]\n", argv[0]);
    return 0;
  }

  struct tinywl_server server = {0};
  /* The Wayland display is managed by libwayland. It handles accepting
   * clients from the Unix socket, manging Wayland globals, and so on. */
  server.wl_display = wl_display_create();
  /* The backend is a wlroots feature which abstracts the underlying input and
   * output hardware. The autocreate option will choose the most suitable
   * backend based on the current environment, such as opening an X11 window
   * if an X11 server is running. */
  server.backend = wlr_backend_autocreate(
      wl_display_get_event_loop(server.wl_display), NULL);
  if (server.backend == NULL) {
    wlr_log(WLR_ERROR, "failed to create wlr_backend");
    return 1;
  }

  /* Renderer selection is deliberately not environment-driven: Vulkan is the
   * sole production data path and capability failures abort startup. */
  int drm_fd = wlr_backend_get_drm_fd(server.backend);
  if (drm_fd < 0) {
    wlr_log(WLR_ERROR, "Vulkan renderer requires a DRM backend FD");
    wlr_backend_destroy(server.backend);
    wl_display_destroy(server.wl_display);
    return 1;
  }
  server.renderer = os_vk_renderer_create_with_drm_fd(drm_fd);
  if (server.renderer == NULL) {
    wlr_log(WLR_ERROR, "failed to create production Vulkan renderer");
    wlr_backend_destroy(server.backend);
    wl_display_destroy(server.wl_display);
    return 1;
  }

  /* Only wl_shm is public in phase 2. Client linux-dmabuf remains disabled. */
  if (!wlr_renderer_init_wl_shm(server.renderer, server.wl_display)) {
    wlr_log(WLR_ERROR, "failed to initialize wl_shm for Vulkan renderer");
    wlr_renderer_destroy(server.renderer);
    wlr_backend_destroy(server.backend);
    wl_display_destroy(server.wl_display);
    return 1;
  }

  /* Use the standard dma-buf bridge: Vulkan imports the udmabuf fd and the
   * DRM backend imports that same backing into GEM for KMS scanout. */
  server.allocator = wlr_udmabuf_allocator_create();
  if (server.allocator == NULL) {
    wlr_log(WLR_ERROR, "failed to create udmabuf/PRIME allocator");
    wlr_renderer_destroy(server.renderer);
    wlr_backend_destroy(server.backend);
    wl_display_destroy(server.wl_display);
    return 1;
  }
  if (!os_vulkan_prime_probe(server.renderer, server.allocator, drm_fd)) {
    wlr_log(WLR_ERROR, "Vulkan PRIME capability gate failed");
    wlr_allocator_destroy(server.allocator);
    wlr_renderer_destroy(server.renderer);
    wlr_backend_destroy(server.backend);
    wl_display_destroy(server.wl_display);
    return 1;
  }

  /* This creates some hands-off wlroots interfaces. The compositor is
   * necessary for clients to allocate surfaces, the subcompositor allows to
   * assign the role of subsurfaces to surfaces and the data device manager
   * handles the clipboard. Each of these wlroots interfaces has room for you
   * to dig your fingers in and play with their behavior if you want. Note that
   * the clients cannot set the selection directly without compositor approval,
   * see the handling of the request_set_selection event below.*/
  wlr_compositor_create(server.wl_display, 5, server.renderer);
  wlr_subcompositor_create(server.wl_display);
  wlr_data_device_manager_create(server.wl_display);

  /* Creates an output layout, which a wlroots utility for working with an
   * arrangement of screens in a physical layout. */
  server.output_layout = wlr_output_layout_create(server.wl_display);

  /* Configure a listener to be notified when new outputs are available on the
   * backend. */
  wl_list_init(&server.outputs);
  wl_list_init(&server.layer_surfaces);
  server.new_output.notify = server_new_output;
  wl_signal_add(&server.backend->events.new_output, &server.new_output);

  /* Create a scene graph. This is a wlroots abstraction that handles all
   * rendering and damage tracking. All the compositor author needs to do
   * is add things that should be rendered to the scene graph at the proper
   * positions and then call wlr_scene_output_commit() to render a frame if
   * necessary.
   */
  /* Clients only expose wl_shm buffers in phase 2, so direct scan-out probes
   * cannot succeed until the client linux-dmabuf path is enabled. */
  setenv("WLR_SCENE_DISABLE_DIRECT_SCANOUT", "1", true);
  server.scene = wlr_scene_create();
  server.scene_layout =
      wlr_scene_attach_output_layout(server.scene, server.output_layout);
  server.layer_background = wlr_scene_tree_create(&server.scene->tree);
  server.layer_bottom = wlr_scene_tree_create(&server.scene->tree);
  server.layer_toplevel = wlr_scene_tree_create(&server.scene->tree);
  server.layer_overlay = wlr_scene_tree_create(&server.scene->tree);
  wlr_log(WLR_INFO,
          "phase1.baseline scene=root/background/bottom/toplevel/overlay "
          "renderer=vulkan full_redraw=true client_dmabuf=false");

  server.layer_shell = wlr_layer_shell_v1_create(server.wl_display, 4);
  server.new_layer_surface.notify = server_new_layer_surface;
  wl_signal_add(&server.layer_shell->events.new_surface,
                &server.new_layer_surface);
  server.effect_server = os_effect_server_create(server.wl_display);
  if (server.effect_server == NULL) {
    wlr_log(WLR_ERROR, "failed to create trusted effect global");
    return 1;
  }

  server.decoration_manager =
      wlr_xdg_decoration_manager_v1_create(server.wl_display);
  server.new_decoration.notify = server_new_decoration;
  wl_signal_add(&server.decoration_manager->events.new_toplevel_decoration,
                &server.new_decoration);

  /* Set up xdg-shell version 3. The xdg-shell is a Wayland protocol which is
   * used for application windows. For more detail on shells, refer to
   * https://drewdevault.com/2018/07/29/Wayland-shells.html.
   */
  wl_list_init(&server.toplevels);
  server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 3);
  server.new_xdg_toplevel.notify = server_new_xdg_toplevel;
  wl_signal_add(&server.xdg_shell->events.new_toplevel,
                &server.new_xdg_toplevel);
  server.new_xdg_popup.notify = server_new_xdg_popup;
  wl_signal_add(&server.xdg_shell->events.new_popup, &server.new_xdg_popup);

  /*
   * Creates a cursor, which is a wlroots utility for tracking the cursor
   * image shown on screen.
   */
  server.cursor = wlr_cursor_create();
  wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

  /* Creates an xcursor manager, another wlroots utility which loads up
   * Xcursor themes to source cursor images from and makes sure that cursor
   * images are available at all scale factors on the screen (necessary for
   * HiDPI support). */
  server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);

  /* Load the Apple-style cursor PNGs into cached self-built wlr_buffers.
   * Self-built buffers need no allocator/renderer, so this is safe before the
   * backend starts. Cursors that fail to load fall back to the wlroots default
   * at apply time; the compositor never aborts. */
  os_cursor_init();

  /*
   * wlr_cursor *only* displays an image on screen. It does not move around
   * when the pointer moves. However, we can attach input devices to it, and
   * it will generate aggregate events for all of them. In these events, we
   * can choose how we want to process them, forwarding them to clients and
   * moving the cursor around. More detail on this process is described in
   * https://drewdevault.com/2018/07/17/Input-handling-in-wlroots.html.
   *
   * And more comments are sprinkled throughout the notify functions above.
   */
  server.cursor_mode = TINYWL_CURSOR_PASSTHROUGH;
  server.cursor_motion.notify = server_cursor_motion;
  wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
  server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
  wl_signal_add(&server.cursor->events.motion_absolute,
                &server.cursor_motion_absolute);
  server.cursor_button.notify = server_cursor_button;
  wl_signal_add(&server.cursor->events.button, &server.cursor_button);
  server.cursor_axis.notify = server_cursor_axis;
  wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
  server.cursor_frame.notify = server_cursor_frame;
  wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

  /*
   * Configures a seat, which is a single "seat" at which a user sits and
   * operates the computer. This conceptually includes up to one keyboard,
   * pointer, touch, and drawing tablet device. We also rig up a listener to
   * let us know when new input devices are available on the backend.
   */
  wl_list_init(&server.keyboards);
  server.new_input.notify = server_new_input;
  wl_signal_add(&server.backend->events.new_input, &server.new_input);
  server.seat = wlr_seat_create(server.wl_display, "seat0");
  server.request_cursor.notify = seat_request_cursor;
  wl_signal_add(&server.seat->events.request_set_cursor,
                &server.request_cursor);
  server.request_set_selection.notify = seat_request_set_selection;
  wl_signal_add(&server.seat->events.request_set_selection,
                &server.request_set_selection);

  /* Start the backend. This will enumerate outputs and inputs, become the DRM
   * master, etc */
  if (!wlr_backend_start(server.backend)) {
    wlr_backend_destroy(server.backend);
    wl_display_destroy(server.wl_display);
    return 1;
  }

  /* Outputs have now exercised primary swapchain creation. Do not expose a
   * client socket until both the PRIME probe and backend startup succeeded. */
  const char *socket = wl_display_add_socket_auto(server.wl_display);
  if (!socket) {
    wlr_log(WLR_ERROR, "failed to create Wayland socket");
    wlr_backend_destroy(server.backend);
    wl_display_destroy(server.wl_display);
    return 1;
  }

  /* Bootstrap the cursor: warp it to the centre of the output layout and arm
   * the left_ptr image, so the software cursor is visible immediately even
   * before any pointer motion arrives (the kernel USB-mouse path may not yet
   * deliver events). Without this wlr_cursor sits at (0,0) with no image and
   * nothing renders. warp_absolute(NULL, 0.5, 0.5) maps to the full layout box
   * and jumps to its midpoint; os_cursor_apply arms the buffer. */
  wlr_cursor_warp_absolute(server.cursor, NULL, 0.5, 0.5);
  os_cursor_apply(server.cursor, server.cursor_mgr, "left_ptr");

  /* Set the WAYLAND_DISPLAY environment variable to our socket and run the
   * startup command if requested. */
  setenv("WAYLAND_DISPLAY", socket, true);
  (void)syscall(SYS_PERF, XOS_PERF_COUNTER_MARK, XOS_PERF_GUI_COMPOSITOR_READY,
                0, 0, 0, 0);
  if (startup_cmd) {
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0) {
      struct wl_client *trusted =
          wl_client_create(server.wl_display, sockets[0]);
      if (trusted != NULL &&
          os_effect_server_set_trusted_client(server.effect_server, trusted)) {
        pid_t pid = fork();
        if (pid == 0) {
          int flags = fcntl(sockets[1], F_GETFD);
          if (flags >= 0)
            fcntl(sockets[1], F_SETFD, flags & ~FD_CLOEXEC);
          char fd_string[16];
          snprintf(fd_string, sizeof(fd_string), "%d", sockets[1]);
          setenv("WAYLAND_SOCKET", fd_string, true);
          close(sockets[0]);
          execl(startup_cmd, startup_cmd, (void *)NULL);
          _exit(127);
        }
        close(sockets[1]);
        if (pid < 0)
          wl_client_destroy(trusted);
      } else {
        close(sockets[1]);
        if (trusted != NULL)
          wl_client_destroy(trusted);
        else
          close(sockets[0]);
      }
    } else {
      wlr_log(WLR_ERROR, "failed to create trusted WAYLAND_SOCKET: %s",
              strerror(errno));
    }
  }
  /* Run the Wayland event loop. This does not return until you exit the
   * compositor. Starting the backend rigged up all of the necessary event
   * loop configuration to listen to libinput events, DRM events, generate
   * frame events at the refresh rate, and so on. */
  wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s", socket);
  errno = 0;
  wl_display_run(server.wl_display);
  int loop_errno = errno;
  wlr_log(WLR_ERROR, "Wayland event loop stopped: errno=%d (%s)", loop_errno,
          loop_errno ? strerror(loop_errno) : "none");

  /* Once wl_display_run returns, we destroy all clients then shut down the
   * server. */
  wl_display_destroy_clients(server.wl_display);
  os_effect_server_destroy(server.effect_server);
  wlr_scene_node_destroy(&server.scene->tree.node);
  wlr_xcursor_manager_destroy(server.cursor_mgr);
  wlr_cursor_destroy(server.cursor);
  os_cursor_fini();
  wlr_allocator_destroy(server.allocator);
  wlr_renderer_destroy(server.renderer);
  wlr_backend_destroy(server.backend);
  size_t live = os_lifetime_total(&server.lifetime);
  wlr_log(live == 0 ? WLR_INFO : WLR_ERROR,
          "phase1.shutdown live_resources=%zu", live);
  wl_display_destroy(server.wl_display);
  return 0;
}

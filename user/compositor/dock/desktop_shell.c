/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <png.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "os-effect-v1-client-protocol.h"
#include "wlr-layer-shell-client-protocol.h"

#include "dock/desktop_shell.h"
#include "dock/dock.h"

#define MAX_OUTPUTS 8

struct image {
  uint32_t *pixels;
  int width, height;
};

struct shell_output {
  struct wl_output *output;
  struct wl_surface *background;
  struct zwlr_layer_surface_v1 *background_layer;
  struct os_dock dock;
  struct os_surface_effect_v1 *effect;
  int background_width, background_height;
  int mode_width, mode_height;
  int scale;
};

struct app {
  struct wl_display *display;
  struct wl_compositor *compositor;
  struct wl_shm *shm;
  struct zwlr_layer_shell_v1 *layer_shell;
  struct os_effect_manager_v1 *effect_manager;
  struct wl_seat *seat;
  struct wl_pointer *pointer;
  struct wl_surface *pointer_surface;
  struct shell_output outputs[MAX_OUTPUTS];
  int output_count;
  struct image wallpaper;
  bool running;
};

struct shm_buffer {
  struct wl_buffer *buffer;
  void *data;
  size_t size;
};

static struct app app = {.running = true};

static bool load_png(const char *path, struct image *image) {
  FILE *fp = fopen(path, "rb");
  if (fp == NULL)
    return false;
  png_structp png =
      png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  png_infop info = png_create_info_struct(png);
  uint8_t *rgba = NULL;
  png_bytep *rows = NULL;
  if (png == NULL || info == NULL || setjmp(png_jmpbuf(png))) {
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    free(rgba);
    free(rows);
    return false;
  }
  png_init_io(png, fp);
  png_read_info(png, info);
  png_uint_32 width = png_get_image_width(png, info);
  png_uint_32 height = png_get_image_height(png, info);
  int color = png_get_color_type(png, info);
  if (png_get_bit_depth(png, info) == 16)
    png_set_strip_16(png);
  if (color == PNG_COLOR_TYPE_PALETTE)
    png_set_palette_to_rgb(png);
  if (color == PNG_COLOR_TYPE_GRAY && png_get_bit_depth(png, info) < 8)
    png_set_expand_gray_1_2_4_to_8(png);
  if (png_get_valid(png, info, PNG_INFO_tRNS))
    png_set_tRNS_to_alpha(png);
  if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA)
    png_set_gray_to_rgb(png);
  if (!(color & PNG_COLOR_MASK_ALPHA))
    png_set_add_alpha(png, 0xff, PNG_FILLER_AFTER);
  png_read_update_info(png, info);
  rgba = malloc((size_t)width * height * 4);
  rows = malloc(sizeof(*rows) * height);
  if (rgba == NULL || rows == NULL)
    goto fail;
  for (png_uint_32 y = 0; y < height; y++)
    rows[y] = rgba + y * width * 4;
  png_read_image(png, rows);
  image->pixels = malloc((size_t)width * height * sizeof(uint32_t));
  if (image->pixels == NULL)
    goto fail;
  for (size_t i = 0; i < (size_t)width * height; i++) {
    uint8_t *p = &rgba[i * 4];
    image->pixels[i] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) |
                       ((uint32_t)p[1] << 8) | p[2];
  }
  image->width = width;
  image->height = height;
  free(rgba);
  free(rows);
  png_destroy_read_struct(&png, &info, NULL);
  fclose(fp);
  return true;
fail:
  free(rgba);
  free(rows);
  free(image->pixels);
  image->pixels = NULL;
  png_destroy_read_struct(&png, &info, NULL);
  fclose(fp);
  return false;
}

static void buffer_release(void *data, struct wl_buffer *buffer) {
  struct shm_buffer *shm_buffer = data;
  wl_buffer_destroy(buffer);
  munmap(shm_buffer->data, shm_buffer->size);
  free(shm_buffer);
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static struct shm_buffer *create_buffer(int width, int height) {
  size_t size = (size_t)width * height * 4;
  int fd = memfd_create("wallpaper", MFD_CLOEXEC);
  if (fd < 0 || ftruncate(fd, size) < 0) {
    if (fd >= 0)
      close(fd);
    return NULL;
  }
  void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (data == MAP_FAILED) {
    close(fd);
    return NULL;
  }
  struct wl_shm_pool *pool = wl_shm_create_pool(app.shm, fd, size);
  struct shm_buffer *buffer = calloc(1, sizeof(*buffer));
  buffer->buffer = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4,
                                             WL_SHM_FORMAT_ARGB8888);
  buffer->data = data;
  buffer->size = size;
  wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer);
  wl_shm_pool_destroy(pool);
  close(fd);
  return buffer;
}

static int clamp_int(int value, int low, int high) {
  if (value < low)
    return low;
  if (value > high)
    return high;
  return value;
}

static uint32_t blend_coverage(uint32_t dst, uint32_t src, int coverage) {
  unsigned int sa = ((src >> 24) & 0xff) * coverage / 16;
  unsigned int sr = ((src >> 16) & 0xff) * coverage / 16;
  unsigned int sg = ((src >> 8) & 0xff) * coverage / 16;
  unsigned int sb = (src & 0xff) * coverage / 16;
  unsigned int da = (dst >> 24) & 0xff;
  unsigned int dr = (dst >> 16) & 0xff;
  unsigned int dg = (dst >> 8) & 0xff;
  unsigned int db = dst & 0xff;
  unsigned int inverse = 255 - sa;
  return ((sa + da * inverse / 255) << 24) | ((sr + dr * inverse / 255) << 16) |
         ((sg + dg * inverse / 255) << 8) | (sb + db * inverse / 255);
}

static int rounded_coverage(int px, int py, int x, int y, int width, int height,
                            int radius) {
  const int samples = 4;
  const int units = samples * 2;
  int left_center = (x + radius) * units;
  int right_center = (x + width - radius) * units;
  int top_center = (y + radius) * units;
  int bottom_center = (y + height - radius) * units;
  int radius_scaled = radius * units;
  int radius_sq = radius_scaled * radius_scaled;
  int coverage = 0;

  for (int sy = 0; sy < samples; sy++) {
    for (int sx = 0; sx < samples; sx++) {
      int sample_x = px * units + 1 + sx * 2;
      int sample_y = py * units + 1 + sy * 2;
      int nearest_x = clamp_int(sample_x, left_center, right_center);
      int nearest_y = clamp_int(sample_y, top_center, bottom_center);
      int dx = sample_x - nearest_x;
      int dy = sample_y - nearest_y;
      if (dx * dx + dy * dy <= radius_sq)
        coverage++;
    }
  }
  return coverage;
}

static void rounded_rect(uint32_t *pixels, int stride, int canvas_height, int x,
                         int y, int width, int height, int radius,
                         uint32_t color) {
  for (int py = y; py < y + height; py++) {
    if (py < 0 || py >= canvas_height)
      continue;
    for (int px = x; px < x + width; px++) {
      if (px < 0 || px >= stride)
        continue;
      int coverage = rounded_coverage(px, py, x, y, width, height, radius);
      if (coverage > 0) {
        int index = py * stride + px;
        pixels[index] = blend_coverage(pixels[index], color, coverage);
      }
    }
  }
}

static void fill_rect(uint32_t *pixels, int stride, int canvas_height, int x,
                      int y, int width, int height, uint32_t color) {
  for (int py = y; py < y + height && py < canvas_height; py++)
    for (int px = x; px < x + width && px < stride; px++)
      if (px >= 0 && py >= 0)
        pixels[py * stride + px] = color;
}

static void draw_wallpaper(struct shell_output *output, int width, int height) {
  struct shm_buffer *buffer = create_buffer(width, height);
  if (buffer == NULL)
    return;
  uint32_t *dst = buffer->data;
  if (app.wallpaper.pixels == NULL) {
    for (int i = 0; i < width * height; i++)
      dst[i] = 0xff496b83;
  } else {
    double scale_x = (double)width / app.wallpaper.width;
    double scale_y = (double)height / app.wallpaper.height;
    double scale = scale_x > scale_y ? scale_x : scale_y;
    double shown_w = app.wallpaper.width * scale;
    double shown_h = app.wallpaper.height * scale;
    double offset_x = (shown_w - width) / 2.0;
    double offset_y = (shown_h - height) / 2.0;
    for (int y = 0; y < height; y++) {
      int sy = (int)((y + offset_y) / scale);
      if (sy >= app.wallpaper.height)
        sy = app.wallpaper.height - 1;
      for (int x = 0; x < width; x++) {
        int sx = (int)((x + offset_x) / scale);
        if (sx >= app.wallpaper.width)
          sx = app.wallpaper.width - 1;
        dst[y * width + x] =
            app.wallpaper.pixels[sy * app.wallpaper.width + sx];
      }
    }
  }
  wl_surface_attach(output->background, buffer->buffer, 0, 0);
  wl_surface_damage_buffer(output->background, 0, 0, width, height);
  wl_surface_commit(output->background);
}

static void draw_dock(struct shell_output *output) {
  int scale = output->scale > 0 ? output->scale : 1;
  int width = output->dock.layout.width * scale;
  int height = output->dock.layout.height * scale;
  struct shm_buffer *buffer = create_buffer(width, height);
  if (buffer == NULL)
    return;
  uint32_t *pixels = buffer->data;
  memset(pixels, 0, buffer->size);

  int top = height / 10;
  int icon_size = output->dock.layout.icon_size * scale;
  int icon_x = (width - icon_size) / 2;
  int content_padding = output->dock.layout.padding * scale;
  int icon_y = top + content_padding;
  int icon_inset = clamp_int(icon_size / 16, 2 * scale, 4 * scale);
  int mark = clamp_int(icon_size / 24, scale, 3 * scale);
  int indicator_h = clamp_int(height / 24, 2 * scale, 3 * scale);
  int indicator_w = height / 9;
  int indicator_x = (width - indicator_w) / 2;
  int indicator_y = height - indicator_h - 2 * scale;

  rounded_rect(pixels, width, height, icon_x, icon_y, icon_size, icon_size,
               icon_size / 4, 0xffd9dce2);
  rounded_rect(pixels, width, height, icon_x + icon_inset, icon_y + icon_inset,
               icon_size - 2 * icon_inset, icon_size - 2 * icon_inset,
               icon_size / 5, 0xff17191f);

  int glyph_x = icon_x + icon_size / 4;
  int glyph_y = icon_y + icon_size / 3;
  int glyph_steps = icon_size / 6;
  for (int i = 0; i < glyph_steps; i++) {
    fill_rect(pixels, width, height, glyph_x + i * mark, glyph_y + i * mark / 2,
              mark, mark, 0xfff4f6f8);
    fill_rect(pixels, width, height, glyph_x + i * mark,
              glyph_y + icon_size / 5 - i * mark / 2, mark, mark, 0xfff4f6f8);
  }
  fill_rect(pixels, width, height, icon_x + icon_size / 2,
            icon_y + icon_size * 3 / 5, icon_size / 4, mark, 0xfff4f6f8);
  rounded_rect(pixels, width, height, indicator_x, indicator_y, indicator_w,
               indicator_h, indicator_h / 2, 0xff2a2b30);
  wl_surface_attach(output->dock.surface, buffer->buffer, 0, 0);
  wl_surface_damage_buffer(output->dock.surface, 0, 0, width, height);
  wl_surface_commit(output->dock.surface);
}

static void background_configure(void *data,
                                 struct zwlr_layer_surface_v1 *layer,
                                 uint32_t serial, uint32_t width,
                                 uint32_t height) {
  struct shell_output *output = data;
  zwlr_layer_surface_v1_ack_configure(layer, serial);
  if ((int)width != output->background_width ||
      (int)height != output->background_height) {
    output->background_width = width;
    output->background_height = height;
    draw_wallpaper(output, width, height);
  }
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer) {
  (void)data;
  (void)layer;
  app.running = false;
}

static const struct zwlr_layer_surface_v1_listener background_listener = {
    .configure = background_configure,
    .closed = layer_closed,
};

static void render_dock(void *data) { draw_dock(data); }

static void dock_closed(void *data) {
  (void)data;
  app.running = false;
}

static void create_output_surfaces(struct shell_output *output) {
  if (output->background != NULL || app.layer_shell == NULL ||
      app.compositor == NULL)
    return;
  output->background = wl_compositor_create_surface(app.compositor);
  output->background_layer = zwlr_layer_shell_v1_get_layer_surface(
      app.layer_shell, output->background, output->output,
      ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "desktop-wallpaper");
  zwlr_layer_surface_v1_set_anchor(output->background_layer,
                                   ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                       ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
                                       ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                       ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
  zwlr_layer_surface_v1_set_exclusive_zone(output->background_layer, -1);
  zwlr_layer_surface_v1_add_listener(output->background_layer,
                                     &background_listener, output);
  wl_surface_commit(output->background);

  int scale = output->scale > 0 ? output->scale : 1;
  int width = output->mode_width > 0 ? output->mode_width / scale : 1280;
  int height = output->mode_height > 0 ? output->mode_height / scale : 720;
  if (!os_dock_create(&output->dock, app.compositor, app.layer_shell,
                      output->output, width, height, scale, render_dock,
                      dock_closed, output))
    app.running = false;
  if (output->dock.surface != NULL && app.effect_manager != NULL) {
    output->effect = os_effect_manager_v1_get_surface_effect(
        app.effect_manager, output->dock.surface);
    int backdrop_top = output->dock.layout.height / 10;
    os_surface_effect_v1_set_blur(
        output->effect, 1, backdrop_top, output->dock.layout.width - 2,
        output->dock.layout.height - backdrop_top, 12);
    os_surface_effect_v1_set_tint(output->effect, 0xffffff08);
    wl_surface_commit(output->dock.surface);
  }
}

static void output_geometry(void *data, struct wl_output *output, int32_t x,
                            int32_t y, int32_t pw, int32_t ph, int32_t subpixel,
                            const char *make, const char *model,
                            int32_t transform) {
  (void)data;
  (void)output;
  (void)x;
  (void)y;
  (void)pw;
  (void)ph;
  (void)subpixel;
  (void)make;
  (void)model;
  (void)transform;
}
static void output_mode(void *data, struct wl_output *output, uint32_t flags,
                        int32_t width, int32_t height, int32_t refresh) {
  struct shell_output *shell_output = data;
  (void)output;
  (void)refresh;
  if (flags & WL_OUTPUT_MODE_CURRENT) {
    shell_output->mode_width = width;
    shell_output->mode_height = height;
  }
}
static void output_done(void *data, struct wl_output *output) {
  (void)data;
  (void)output;
}
static void output_scale(void *data, struct wl_output *output, int32_t factor) {
  struct shell_output *shell_output = data;
  (void)output;
  shell_output->scale = factor > 0 ? factor : 1;
}
static void output_name(void *data, struct wl_output *output,
                        const char *name) {
  (void)data;
  (void)output;
  (void)name;
}
static void output_description(void *data, struct wl_output *output,
                               const char *description) {
  (void)data;
  (void)output;
  (void)description;
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

static void pointer_enter(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t x, wl_fixed_t y) {
  (void)data;
  (void)pointer;
  (void)serial;
  (void)x;
  (void)y;
  app.pointer_surface = surface;
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface) {
  (void)data;
  (void)pointer;
  (void)serial;
  (void)surface;
  app.pointer_surface = NULL;
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
                           uint32_t time, wl_fixed_t x, wl_fixed_t y) {
  (void)data;
  (void)pointer;
  (void)time;
  (void)x;
  (void)y;
}

static void spawn_terminal(bool run_autotest) {
  pid_t pid = fork();
  if (pid == 0) {
    if (run_autotest)
      setenv("XOS_AUTOTEST", "1", 1);
    execl("/usr/bin/terminal", "/usr/bin/terminal", (char *)NULL);
    _exit(127);
  }
}

static void pointer_button(void *data, struct wl_pointer *pointer,
                           uint32_t serial, uint32_t time, uint32_t button,
                           uint32_t state) {
  (void)data;
  (void)pointer;
  (void)serial;
  (void)time;
  if (button != BTN_LEFT || state != WL_POINTER_BUTTON_STATE_PRESSED)
    return;
  for (int i = 0; i < app.output_count; i++) {
    if (!os_dock_owns_surface(&app.outputs[i].dock, app.pointer_surface))
      continue;
    spawn_terminal(false);
    break;
  }
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
                         uint32_t axis, wl_fixed_t value) {
  (void)data;
  (void)pointer;
  (void)time;
  (void)axis;
  (void)value;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
};

static void seat_capabilities(void *data, struct wl_seat *seat,
                              uint32_t capabilities) {
  (void)data;
  if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && app.pointer == NULL) {
    app.pointer = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(app.pointer, &pointer_listener, NULL);
  }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
  (void)data;
  (void)seat;
  (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version) {
  (void)data;
  if (strcmp(interface, wl_compositor_interface.name) == 0) {
    app.compositor =
        wl_registry_bind(registry, name, &wl_compositor_interface, 4);
  } else if (strcmp(interface, wl_shm_interface.name) == 0) {
    app.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
  } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
    app.layer_shell =
        wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface,
                         version < 4 ? version : 4);
  } else if (strcmp(interface, os_effect_manager_v1_interface.name) == 0) {
    app.effect_manager =
        wl_registry_bind(registry, name, &os_effect_manager_v1_interface, 1);
  } else if (strcmp(interface, wl_seat_interface.name) == 0) {
    app.seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    wl_seat_add_listener(app.seat, &seat_listener, NULL);
  } else if (strcmp(interface, wl_output_interface.name) == 0 &&
             app.output_count < MAX_OUTPUTS) {
    struct shell_output *output = &app.outputs[app.output_count++];
    output->scale = 1;
    output->output = wl_registry_bind(registry, name, &wl_output_interface,
                                      version < 4 ? version : 4);
    wl_output_add_listener(output->output, &output_listener, output);
  }
}

static void registry_remove(void *data, struct wl_registry *registry,
                            uint32_t name) {
  (void)data;
  (void)registry;
  (void)name;
}
static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

int os_desktop_shell_run(int argc, char **argv) {
  (void)argc;
  (void)argv;
  const char *wallpaper = getenv("TINYWL_WALLPAPER");
  if (wallpaper == NULL)
    wallpaper = "/usr/share/wallpaper.png";
  if (!load_png(wallpaper, &app.wallpaper))
    fprintf(stderr, "desktop-shell: cannot load %s, using fallback\n",
            wallpaper);
  signal(SIGCHLD, SIG_IGN);
  app.display = wl_display_connect(NULL);
  if (app.display == NULL)
    return 1;
  struct wl_registry *registry = wl_display_get_registry(app.display);
  wl_registry_add_listener(registry, &registry_listener, NULL);
  wl_display_roundtrip(app.display);
  if (app.compositor == NULL || app.shm == NULL || app.layer_shell == NULL ||
      app.effect_manager == NULL) {
    fprintf(stderr, "desktop-shell: compositor lacks wl_shm or layer-shell\n");
    return 1;
  }
  /* The first roundtrip discovers outputs; the second receives mode/scale. */
  wl_display_roundtrip(app.display);
  for (int i = 0; i < app.output_count; i++)
    create_output_surfaces(&app.outputs[i]);
  // Only the desktop's initial terminal owns the automatic test run. Further
  // terminals are ordinary interactive sessions, even in a TEST image.
  spawn_terminal(true);
  wl_display_flush(app.display);
  while (app.running && wl_display_dispatch(app.display) >= 0) {
  }
  for (int i = 0; i < app.output_count; ++i) {
    if (app.outputs[i].effect != NULL)
      os_surface_effect_v1_destroy(app.outputs[i].effect);
    os_dock_destroy(&app.outputs[i].dock);
  }
  if (app.effect_manager != NULL)
    os_effect_manager_v1_destroy(app.effect_manager);
  free(app.wallpaper.pixels);
  wl_display_disconnect(app.display);
  return 0;
}

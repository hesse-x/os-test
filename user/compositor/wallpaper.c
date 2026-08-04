/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <png.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-layer-shell-client-protocol.h"

#define MAX_OUTPUTS 8

struct image {
  uint32_t *pixels;
  int width, height;
};

struct shell_output {
  struct wl_output *output;
  struct wl_surface *background;
  struct zwlr_layer_surface_v1 *background_layer;
  int background_width, background_height;
};

struct app {
  struct wl_display *display;
  struct wl_compositor *compositor;
  struct wl_shm *shm;
  struct zwlr_layer_shell_v1 *layer_shell;
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
  (void)data;
  (void)output;
  (void)flags;
  (void)width;
  (void)height;
  (void)refresh;
}
static void output_done(void *data, struct wl_output *output) {
  (void)data;
  (void)output;
}
static void output_scale(void *data, struct wl_output *output, int32_t factor) {
  (void)data;
  (void)output;
  (void)factor;
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
  } else if (strcmp(interface, wl_output_interface.name) == 0 &&
             app.output_count < MAX_OUTPUTS) {
    struct shell_output *output = &app.outputs[app.output_count++];
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

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  const char *wallpaper = getenv("TINYWL_WALLPAPER");
  if (wallpaper == NULL)
    wallpaper = "/usr/share/wallpaper.png";
  if (!load_png(wallpaper, &app.wallpaper))
    fprintf(stderr, "wallpaper: cannot load %s, using fallback\n", wallpaper);
  app.display = wl_display_connect(NULL);
  if (app.display == NULL)
    return 1;
  struct wl_registry *registry = wl_display_get_registry(app.display);
  wl_registry_add_listener(registry, &registry_listener, NULL);
  wl_display_roundtrip(app.display);
  if (app.compositor == NULL || app.shm == NULL || app.layer_shell == NULL) {
    fprintf(stderr, "wallpaper: compositor lacks wl_shm or layer-shell\n");
    return 1;
  }
  for (int i = 0; i < app.output_count; i++)
    create_output_surfaces(&app.outputs[i]);
  wl_display_flush(app.display);
  while (app.running && wl_display_dispatch(app.display) >= 0) {
  }
  free(app.wallpaper.pixels);
  wl_display_disconnect(app.display);
  return 0;
}

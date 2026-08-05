/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * Apple-style software cursor loading for tinywl. PNGs in user/cursor/ are
 * decoded to straight ARGB8888 (reusing wallpaper.c's libpng decoder), then
 * pre-multiplied in place, then wrapped in self-built wlr_buffers (malloc'd
 * ARGB + wlr_buffer_init with a DATA_PTR impl). wlr_cursor_set_buffer feeds
 * them through wlroots' software-cursor GLES2 texture-quad path — the same
 * path the default xcursor uses, only the pixel source differs.
 *
 * This is the only translation unit that includes cursors.h: os_cursors[] is
 * declared static there, so a second includer would be a duplicate-definition
 * link error.
 */
#define _GNU_SOURCE
#include <drm/drm_fourcc.h>
#include <png.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>

#include "cursor.h"
#include "cursors.h"

/* Cached buffer for one cursor. The ARGB pixels are owned by the buffer and
 * freed in its destroy handler. */
struct os_cursor_buf {
  struct wlr_buffer base;
  uint32_t *data;
  size_t stride;
};

/* One entry per os_cursors[]; buf stays NULL when loading failed. The table
 * currently has 55 name entries (44 unique PNGs), so 64 leaves headroom. */
static struct wlr_buffer *os_cursor_buffers[64];

/* ---- libpng decode → straight ARGB8888 (mirrors wallpaper.c:load_png) ---- */
static bool load_cursor_png(const char *path, uint32_t **out_argb, int *w,
                            int *h) {
  FILE *fp = fopen(path, "rb");
  if (fp == NULL) {
    return false;
  }
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
  if (png_get_bit_depth(png, info) == 16) {
    png_set_strip_16(png);
  }
  if (color == PNG_COLOR_TYPE_PALETTE) {
    png_set_palette_to_rgb(png);
  }
  if (color == PNG_COLOR_TYPE_GRAY && png_get_bit_depth(png, info) < 8) {
    png_set_expand_gray_1_2_4_to_8(png);
  }
  if (png_get_valid(png, info, PNG_INFO_tRNS)) {
    png_set_tRNS_to_alpha(png);
  }
  if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA) {
    png_set_gray_to_rgb(png);
  }
  if (!(color & PNG_COLOR_MASK_ALPHA)) {
    png_set_add_alpha(png, 0xff, PNG_FILLER_AFTER);
  }
  png_read_update_info(png, info);
  rgba = malloc((size_t)width * height * 4);
  rows = malloc(sizeof(*rows) * height);
  if (rgba == NULL || rows == NULL) {
    goto fail;
  }
  for (png_uint_32 y = 0; y < height; y++) {
    rows[y] = rgba + y * width * 4;
  }
  png_read_image(png, rows);
  uint32_t *argb = malloc((size_t)width * height * sizeof(uint32_t));
  if (argb == NULL) {
    goto fail;
  }
  for (size_t i = 0; i < (size_t)width * height; i++) {
    uint8_t *p = &rgba[i * 4];
    argb[i] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) |
              ((uint32_t)p[1] << 8) | p[2];
  }
  *out_argb = argb;
  *w = (int)width;
  *h = (int)height;
  free(rgba);
  free(rows);
  png_destroy_read_struct(&png, &info, NULL);
  fclose(fp);
  return true;
fail:
  free(rgba);
  free(rows);
  png_destroy_read_struct(&png, &info, NULL);
  fclose(fp);
  return false;
}

/* straight ARGB8888 → pre-multiplied ARGB8888, in place. Transparent pixels
 * are zeroed whole so no tinted fringe leaks at edges. */
static void png_to_premult_argb(uint32_t *argb, size_t n) {
  for (size_t i = 0; i < n; i++) {
    uint32_t px = argb[i];
    uint32_t a = px >> 24;
    if (a == 0) {
      argb[i] = 0;
      continue;
    }
    uint32_t r = (px >> 16) & 0xff;
    uint32_t g = (px >> 8) & 0xff;
    uint32_t b = px & 0xff;
    argb[i] = (a << 24) | ((r * a / 255) << 16) | ((g * a / 255) << 8) |
              (b * a / 255);
  }
}

static void os_cursor_buffer_destroy(struct wlr_buffer *buf) {
  struct os_cursor_buf *self = (struct os_cursor_buf *)buf;
  free(self->data);
  free(self);
}

static bool os_cursor_buffer_begin_data_ptr_access(struct wlr_buffer *buf,
                                                   uint32_t flags, void **data,
                                                   uint32_t *format,
                                                   size_t *stride) {
  (void)flags;
  struct os_cursor_buf *self = (struct os_cursor_buf *)buf;
  *data = self->data;
  *format = DRM_FORMAT_ARGB8888;
  *stride = self->stride;
  return true;
}

static void os_cursor_buffer_end_data_ptr_access(struct wlr_buffer *buf) {
  (void)buf;
}

static const struct wlr_buffer_impl os_cursor_buffer_impl = {
    .destroy = os_cursor_buffer_destroy,
    .begin_data_ptr_access = os_cursor_buffer_begin_data_ptr_access,
    .end_data_ptr_access = os_cursor_buffer_end_data_ptr_access,
};

/* Build a self-built wlr_buffer taking ownership of <argb>. Returns NULL on
 * allocation failure (caller retains ownership of argb then). */
static struct wlr_buffer *os_cursor_buffer_create(uint32_t *argb, int w,
                                                  int h) {
  struct os_cursor_buf *buf = calloc(1, sizeof(*buf));
  if (buf == NULL) {
    return NULL;
  }
  wlr_buffer_init(&buf->base, &os_cursor_buffer_impl, w, h);
  buf->data = argb;
  buf->stride = (size_t)w * 4;
  return &buf->base;
}

void os_cursor_init(void) {
  /* os_cursor_buffers[] is zero-initialised; any entry left NULL falls back to
   * the wlroots default cursor at apply time. */
  for (int i = 0; i < os_cursors_n; i++) {
    uint32_t *argb = NULL;
    int w = 0, h = 0;
    if (!load_cursor_png(os_cursors[i].path, &argb, &w, &h)) {
      wlr_log(WLR_ERROR, "cursor: cannot load %s", os_cursors[i].path);
      continue;
    }
    if (w != 128 || h != 128) {
      /* Assets are authored at 128×128 and rendered at scale 4.0 (→32 logical
       * px); a mismatch means the hotspot (in 128px coords) would be off. Load
       * anyway but flag it loudly. */
      wlr_log(WLR_ERROR, "cursor: %s is %dx%d, expected 128x128",
              os_cursors[i].name, w, h);
    }
    png_to_premult_argb(argb, (size_t)w * h);
    struct wlr_buffer *buf = os_cursor_buffer_create(argb, w, h);
    if (buf == NULL) {
      free(argb);
      wlr_log(WLR_ERROR, "cursor: out of memory for %s", os_cursors[i].name);
      continue;
    }
    os_cursor_buffers[i] = buf;
    wlr_log(WLR_INFO, "cursor: loaded %s (%dx%d)", os_cursors[i].name, w, h);
  }
}

void os_cursor_apply(struct wlr_cursor *cursor, struct wlr_xcursor_manager *mgr,
                     const char *name) {
  if (cursor == NULL) {
    return;
  }
  for (int i = 0; i < os_cursors_n; i++) {
    if (strcmp(os_cursors[i].name, name) == 0) {
      struct wlr_buffer *buf = os_cursor_buffers[i];
      if (buf != NULL) {
        int hx = os_cursors[i].hotspot_x;
        int hy = os_cursors[i].hotspot_y;
        if (hx < 0) {
          hx = 0;
        }
        if (hy < 0) {
          hy = 0;
        }
        /* scale 4.0: assets are 128px, displayed at 32 logical px
         * (wlr_cursor divides buffer size by scale). Hotspots are in 128px
         * asset coords and are multiplied by the output scale (1.0) upstream,
         * so they pass through unchanged. */
        wlr_cursor_set_buffer(cursor, buf, hx, hy, 4.0f);
        return;
      }
      break;
    }
  }
  /* Name unknown or its buffer failed to load: fall back to the wlroots
   * default (NULL theme → built-in basic arrow). */
  wlr_cursor_set_xcursor(cursor, mgr, "default");
}

void os_cursor_fini(void) {
  for (int i = 0; i < os_cursors_n; i++) {
    if (os_cursor_buffers[i] != NULL) {
      wlr_buffer_drop(os_cursor_buffers[i]);
      os_cursor_buffers[i] = NULL;
    }
  }
}

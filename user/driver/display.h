/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DRIVER_DISPLAY_H
#define DRIVER_DISPLAY_H

/* Userspace display client API — DRM/KMS backend (virtio-gpu).
   API surface unchanged from the old bochs-display KMS version:
   terminal.cc calls display_client_init / render_cell / clear /
   scroll_up / set_cursor / flush. Internals now drive DRM ioctls
  on a libseat-granted /dev/dri/card0 fd (CREATE_DUMB / MAP_DUMB / mmap / ADDFB
  / SETCRTC / PAGE_FLIP). */

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "user/driver/font.h"
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <xos/ipc.h>

#include "xf86drm.h"
#include "xf86drmMode.h"

/* Local metadata (filled by display_client_init) */
static uint32_t display_pitch;
static uint32_t display_fb_width;
static uint32_t display_fb_height;
static uint32_t display_rows;
static uint32_t display_cols;
static uint8_t *display_back_buffer;
static size_t display_buffer_size;
static int display_dev_fd = -1;

/* DRM handles (internal) */
static uint32_t drm_dumb_handle;
static uint32_t drm_fb_id;

static inline void display_client_destroy(void) {
  if (display_dev_fd < 0)
    return;
  if (drm_fb_id != 0)
    (void)drmModeRmFB(display_dev_fd, drm_fb_id);
  if (display_back_buffer != NULL && display_buffer_size != 0)
    (void)munmap(display_back_buffer, display_buffer_size);
  if (drm_dumb_handle != 0) {
    struct drm_mode_destroy_dumb destroy = {.handle = drm_dumb_handle};
    (void)drmIoctl(display_dev_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
  }

  display_back_buffer = NULL;
  display_buffer_size = 0;
  drm_fb_id = 0;
  drm_dumb_handle = 0;
  display_dev_fd = -1;
}

/* Initialize KMS on a card fd granted by the terminal's libseat session. */
static inline int display_client_init(int fd) {
  if (fd < 0)
    return -1;
  display_dev_fd = fd;

  /* seatd acquires master before passing this open-file-description. */
  if (drmSetMaster(fd) < 0) {
    printf("display_client_init: seatd fd is not DRM master\n");
    display_client_destroy();
    return -1;
  }

  /* Query resources via libdrm */
  drmModeRes *res = drmModeGetResources(fd);
  if (!res) {
    printf("display_client_init: drmModeGetResources failed\n");
    display_client_destroy();
    return -1;
  }

  /* Get the first connector to grab mode info */
  drmModeConnector *conn = NULL;
  for (int i = 0; i < res->count_connectors; i++) {
    conn = drmModeGetConnector(fd, res->connectors[i]);
    if (conn && conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0)
      break;
    drmModeFreeConnector(conn);
    conn = NULL;
  }
  if (!conn) {
    printf("display_client_init: no connected connector\n");
    drmModeFreeResources(res);
    display_client_destroy();
    return -1;
  }

  /* Use the preferred mode (first in list) */
  drmModeModeInfo *mode = &conn->modes[0];
  display_fb_width = mode->hdisplay;
  display_fb_height = mode->vdisplay;

  /* CREATE_DUMB via drmIoctl */
  struct drm_mode_create_dumb dumb;
  memset(&dumb, 0, sizeof(dumb));
  dumb.width = display_fb_width;
  dumb.height = display_fb_height;
  dumb.bpp = 32;
  if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb) < 0) {
    printf("display_client_init: CREATE_DUMB failed\n");
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    display_client_destroy();
    return -1;
  }
  drm_dumb_handle = dumb.handle;
  display_buffer_size = dumb.size;
  display_pitch = dumb.pitch;

  display_rows = display_fb_height / FONT_HEIGHT;
  display_cols = display_fb_width / FONT_WIDTH;

  /* MAP_DUMB via drmIoctl */
  struct drm_mode_map_dumb map;
  memset(&map, 0, sizeof(map));
  map.handle = drm_dumb_handle;
  if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
    printf("display_client_init: MAP_DUMB failed\n");
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    display_client_destroy();
    return -1;
  }

  /* mmap back buffer */
  void *buf =
      mmap(NULL, dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
  if (buf == MAP_FAILED) {
    printf("display_client_init: mmap failed\n");
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    display_client_destroy();
    return -1;
  }
  display_back_buffer = (uint8_t *)buf;

  /* ADDFB via drmModeAddFB */
  uint32_t fb_id;
  if (drmModeAddFB(fd, display_fb_width, display_fb_height, 24, 32,
                   display_pitch, drm_dumb_handle, &fb_id) < 0) {
    printf("display_client_init: ADDFB failed\n");
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    display_client_destroy();
    return -1;
  }
  drm_fb_id = fb_id;

  /* SETCRTC via drmModeSetCrtc */
  if (drmModeSetCrtc(fd, 1 /* crtc_id */, fb_id, 0, 0, &conn->connector_id, 1,
                     mode) < 0) {
    printf("display_client_init: SETCRTC failed\n");
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    display_client_destroy();
    return -1;
  }

  drmModeFreeConnector(conn);
  drmModeFreeResources(res);
  return 0;
}

/* Render a single cell to the back buffer */
static inline void display_client_render_cell(uint32_t row, uint32_t col,
                                              uint8_t ch, uint32_t fg,
                                              uint32_t bg) {
  if (ch < FONT_CHARS_START || ch > 0x7E)
    return;

  uint32_t px = col * FONT_WIDTH;
  uint32_t py = row * FONT_HEIGHT;
  const uint8_t *glyph = font8x16[ch - FONT_CHARS_START];

  for (int r = 0; r < FONT_HEIGHT; r++) {
    if (py + r >= display_fb_height)
      break;
    uint8_t bits = glyph[r];
    uint32_t *dst =
        (uint32_t *)(display_back_buffer + (py + r) * display_pitch + px * 4);
    for (int c = 0; c < FONT_WIDTH; c++) {
      dst[c] = (bits & (0x80 >> c)) ? fg : bg;
    }
  }
}

/* Clear back buffer */
static inline void display_client_clear(uint32_t bg) {
  uint32_t total_pixels = display_fb_height * (display_pitch / 4);
  uint32_t *buf = (uint32_t *)display_back_buffer;
  for (uint32_t i = 0; i < total_pixels; i++) {
    buf[i] = bg;
  }
}

/* Scroll back buffer up by one text row */
static inline void display_client_scroll_up(uint32_t bg) {
  uint32_t line_bytes = display_pitch * FONT_HEIGHT;
  uint32_t fb_bytes = display_fb_height * display_pitch;
  uint32_t move_bytes = fb_bytes - line_bytes;

  /* uint64_t bulk move (8-byte stride), byte-wise tail for remainder */
  uint64_t *dst64 = (uint64_t *)display_back_buffer;
  const uint64_t *src64 = (const uint64_t *)(display_back_buffer + line_bytes);
  uint32_t n64 = move_bytes / 8;
  for (uint32_t i = 0; i < n64; i++)
    dst64[i] = src64[i];

  uint32_t tail = move_bytes - n64 * 8;
  if (tail) {
    uint8_t *d = display_back_buffer + n64 * 8;
    const uint8_t *s = display_back_buffer + line_bytes + n64 * 8;
    for (uint32_t i = 0; i < tail; i++)
      d[i] = s[i];
  }

  uint32_t *last_line =
      (uint32_t *)(display_back_buffer + fb_bytes - line_bytes);
  uint32_t pixels_per_line = line_bytes / 4;
  for (uint32_t i = 0; i < pixels_per_line; i++) {
    last_line[i] = bg;
  }
}

/* Cursor (no-op: software cursor rendered by terminal) */
static inline void display_client_set_cursor(uint32_t x, uint32_t y) {
  (void)x;
  (void)y;
}

/* Flush: page-flip the back buffer to the scanout. The dirty-row range
   is ignored for now (the kernel transfers the full frame on flip);
   keeping the argument preserves the call sites in terminal.cc. */
static inline void display_client_flush(uint32_t dirty_row_start,
                                        uint32_t dirty_row_end) {
  (void)dirty_row_start;
  (void)dirty_row_end;
  drmModePageFlip(display_dev_fd, 1 /* crtc_id */, drm_fb_id,
                  0 /* flags — fire-and-forget */, NULL /* user_data */);
}

#endif

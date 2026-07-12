/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * modetest — Complete libdrm API verification program.
 * Tests: drmOpen -> GETRESOURCES -> GETCONNECTOR -> CREATE_DUMB -> ADDFB ->
 *        SETCRTC -> PAGE_FLIP -> cleanup.
 *
 * Dynamic link version (libdrm.so, loaded via ld.so).
 */

#include "drm/drm.h"
#include "drm/drm_mode.h"
#include "xf86drm.h"
#include "xf86drmMode.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <unistd.h>

/* Draw color bars: red, green, blue, white */
static void draw_pattern(uint32_t *buf, int width, int height, int pitch) {
  const int bar_count = 4;
  const int bar_width = width / bar_count;
  uint32_t colors[] = {
      0x0000FF, /* red   (RGB, stored as BGR in fb) */
      0x00FF00, /* green */
      0xFF0000, /* blue  */
      0xFFFFFF, /* white */
  };

  for (int y = 0; y < height; y++) {
    uint32_t *row = (uint32_t *)((uint8_t *)buf + y * pitch);
    for (int x = 0; x < width; x++) {
      int bar = x / bar_width;
      if (bar >= bar_count)
        bar = bar_count - 1;
      row[x] = colors[bar];
    }
  }
}

/* Wait for page flip vblank event (3s timeout) */
static int wait_vblank_event(int fd) {
  struct pollfd pfd;
  pfd.fd = fd;
  pfd.events = POLLIN;
  int ret = poll(&pfd, 1, 3000);
  if (ret <= 0) {
    printf("modetest: PAGE_FLIP poll timeout or error (ret=%d)\n", ret);
    return -1;
  }
  char buf[64];
  ssize_t n = read(fd, buf, sizeof(buf));
  if (n < (ssize_t)sizeof(struct drm_event_vblank)) {
    printf("modetest: read vblank event failed (n=%zd)\n", n);
    return -1;
  }
  struct drm_event_vblank *ev = (struct drm_event_vblank *)buf;
  printf("modetest: vblank event received (seq=%u)\n", ev->sequence);
  return 0;
}

int main(int argc, char **argv) {
  int count_fbs_only = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--count-fbs") == 0)
      count_fbs_only = 1;
    else if (strcmp(argv[i], "--help") == 0) {
      printf("usage: modetest [options]\n");
      printf("  --count-fbs    only print GETRESOURCES info and exit\n");
      printf("  --help         show this help\n");
      return 0;
    }
  }

  /* 1. drmOpen */
  int fd = drmOpen("drm", NULL);
  if (fd < 0) {
    printf("modetest: drmOpen failed (errno=%d), abort\n", errno);
    return 1;
  }
  printf("modetest: drmOpen OK (fd=%d)\n", fd);

  /* 2. drmSetMaster */
  drmSetMaster(fd);

  /* 3. drmModeGetResources */
  drmModeRes *res = drmModeGetResources(fd);
  if (!res) {
    printf("modetest: drmModeGetResources failed\n");
    drmClose(fd);
    return 1;
  }
  printf(
      "modetest: GETRESOURCES: connectors=%d, crtcs=%d, encoders=%d, fbs=%d\n",
      res->count_connectors, res->count_crtcs, res->count_encoders,
      res->count_fbs);

  if (count_fbs_only) {
    drmModeFreeResources(res);
    drmClose(fd);
    return 0;
  }

  /* 4. Find connected connector */
  drmModeConnector *conn = NULL;
  for (int i = 0; i < res->count_connectors; i++) {
    conn = drmModeGetConnector(fd, res->connectors[i]);
    if (conn && conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0)
      break;
    drmModeFreeConnector(conn);
    conn = NULL;
  }
  if (!conn) {
    printf("modetest: no connected connector found\n");
    drmModeFreeResources(res);
    drmClose(fd);
    return 1;
  }
  printf("modetest: connector=%d: connected, %d modes, preferred=%dx%d\n",
         conn->connector_id, conn->count_modes, conn->modes[0].hdisplay,
         conn->modes[0].vdisplay);

  drmModeCrtc *crtc = drmModeGetCrtc(fd, res->crtcs[0]);
  uint32_t crtc_id = res->crtcs[0];

  /* 5. CREATE_DUMB */
  struct drm_mode_create_dumb create = {0};
  create.width = conn->modes[0].hdisplay;
  create.height = conn->modes[0].vdisplay;
  create.bpp = 32;
  int ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
  if (ret < 0) {
    printf("modetest: CREATE_DUMB failed (ret=%d, errno=%d)\n", ret, errno);
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    drmClose(fd);
    return 2;
  }
  printf("modetest: CREATE_DUMB: %dx%d, bpp=%d, pitch=%d, size=%llu\n",
         create.width, create.height, create.bpp, create.pitch,
         (unsigned long long)create.size);

  /* 6. MAP_DUMB + mmap */
  struct drm_mode_map_dumb map = {0};
  map.handle = create.handle;
  ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
  if (ret < 0) {
    printf("modetest: MAP_DUMB failed\n");
    drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &create);
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    drmClose(fd);
    return 2;
  }

  uint32_t *fb_buf = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                          fd, map.offset);
  if (fb_buf == MAP_FAILED) {
    printf("modetest: mmap failed\n");
    drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &create);
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    drmClose(fd);
    return 2;
  }
  printf("modetest: MAP_DUMB OK, mmap at %p\n", (void *)fb_buf);

  /* 7. Draw pattern */
  printf("modetest: draw pattern: 4 color bars\n");
  draw_pattern(fb_buf, create.width, create.height, create.pitch);

  /* 8. ADDFB */
  uint32_t fb_id = 0;
  ret = drmModeAddFB(fd, create.width, create.height, 24, 32, create.pitch,
                     create.handle, &fb_id);
  if (ret < 0) {
    printf("modetest: ADDFB failed (ret=%d, errno=%d)\n", ret, errno);
    munmap(fb_buf, create.size);
    drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &create);
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    drmClose(fd);
    return 3;
  }
  printf("modetest: ADDFB: fb_id=%u\n", fb_id);

  /* 9. SETCRTC */
  ret = drmModeSetCrtc(fd, crtc_id, fb_id, 0, 0, &conn->connector_id, 1,
                       &conn->modes[0]);
  if (ret < 0) {
    printf("modetest: SETCRTC failed (ret=%d, errno=%d)\n", ret, errno);
    drmModeRmFB(fd, fb_id);
    munmap(fb_buf, create.size);
    drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &create);
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    drmClose(fd);
    return 4;
  }
  printf("modetest: SETCRTC: OK (fb=%u, mode=%dx%d)\n", fb_id,
         conn->modes[0].hdisplay, conn->modes[0].vdisplay);

  /* 10. PAGE_FLIP with vblank event */
  printf("modetest: PAGE_FLIP submitted, waiting for vblank...\n");
  drmModePageFlip(fd, crtc_id, fb_id, DRM_MODE_PAGE_FLIP_EVENT, NULL);
  wait_vblank_event(fd);

  /* 11. Cleanup */
  drmModeRmFB(fd, fb_id);
  munmap(fb_buf, create.size);
  drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &create);
  drmModeFreeCrtc(crtc);
  drmModeFreeConnector(conn);
  drmModeFreeResources(res);
  drmDropMaster(fd);
  drmClose(fd);
  printf("modetest: cleanup done, exiting.\n");
  return 0;
}

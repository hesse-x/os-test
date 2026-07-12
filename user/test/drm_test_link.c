/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * libdrm.a link verification (libdrm.md §3.4):
 * calls drmOpen + drmModeGetResources to prove libdrm.a links + runs.
 */

#include "drm/drm.h"
#include "xf86drm.h"
#include "xf86drmMode.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
  int fd = drmOpen("drm", NULL);
  if (fd < 0) {
    printf("drmOpen failed (errno %d), fallback to direct open\n", errno);
    fd = open("/dev/dri/card0", O_RDWR);
  }
  if (fd < 0) {
    perror("open /dev/dri/card0");
    return 1;
  }
  drmModeRes *res = drmModeGetResources(fd);
  if (!res) {
    perror("drmModeGetResources");
    drmClose(fd);
    return 1;
  }
  printf("drmModeGetResources OK: %u connectors, %u crtcs, %u fbs\n",
         res->count_connectors, res->count_crtcs, res->count_fbs);
  drmModeFreeResources(res);
  drmClose(fd);
  return 0;
}

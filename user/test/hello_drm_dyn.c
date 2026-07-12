/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * Minimal libdrm.so composite loading test.
 * Verifies that libdrm.so can be loaded via ld.so without segfault.
 * Does NOT depend on /dev/dri/card0 being available.
 */

#include <stdio.h>
#include <xf86drm.h>

int main(void) {
  printf("hello_drm_dyn: testing libdrm.so loading...\n");
  /* Just verify drmOpen returns something */
  int fd = drmOpen("drm", NULL);
  if (fd < 0) {
    printf("hello_drm_dyn: drmOpen failed (expected, no card0?)\n");
    return 0; /* Not a failure — just tests that .so loads */
  }
  drmClose(fd);
  return 0;
}

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// test_pixman_smoke — target-side smoke test for pixman, a wlroots
// prerequisite.
//
// Exercises only the pure-compute paths: image creation, region ops, fill.
// Touches no device and no wlroots rendering. Proves build/libpixman-1.so can
// be loaded by the musl loader, its symbols resolve, and the minimal API
// behaves correctly. This is the runtime verification for WF-3 (pixman
// port), matching the build.sh staging artifacts. No Unity — each step prints
// a marker and any failure _exit's non-zero, and test_runner reports [FAIL].
// Follows the diagnostic style of test_egl_smoke.c.

#include <pixman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fail(const char *what) {
  printf("[FAIL] pixman_smoke: %s\n", what);
  _exit(1);
}

int main(void) {
  printf("pixman: version %s\n", pixman_version_string());

  // 1. Create an a8r8g8b8 bitmap image (8x8); a non-zero first byte proves
  // the allocation succeeded.
  pixman_image_t *img =
      pixman_image_create_bits(PIXMAN_a8r8g8b8, 8, 8, NULL, 0);
  if (!img)
    fail("pixman_image_create_bits");
  printf("pixman: created a8r8g8b8 8x8 image %p\n", (void *)img);

  // 2. Fill the whole image with a solid color: pixman_image_fill_rectangles.
  pixman_color_t red;
  red.red = 0xffff;
  red.green = 0;
  red.blue = 0;
  red.alpha = 0xffff;
  pixman_rectangle16_t rect = {0, 0, 8, 8};
  pixman_bool_t ok =
      pixman_image_fill_rectangles(PIXMAN_OP_SRC, img, &red, 1, &rect);
  if (!ok)
    fail("pixman_image_fill_rectangles");
  // The first pixel should be red. PIXMAN_a8r8g8b8 (format 0x00020004) channel
  // order is a@24 r@16 g@8 b@0 (8 bits per channel), so red is 0xFFFF0000.
  // Read as a little-endian uint32, the 4 bytes equal that value (byte 0
  // 0x00=b, next 0x00=g, 0xff=r, 0xff=a).
  uint32_t *data = (uint32_t *)pixman_image_get_data(img);
  uint32_t px = data[0];
  printf("pixman: filled pixel 0x%08x\n", px);
  if (px != 0xffff0000)
    fail("fill_rectangles produced wrong pixel");
  pixman_image_unref(img);

  // 3. Region ops: union of two rectangle regions, check the bounding box.
  pixman_region16_t r1, r2, ru;
  pixman_region_init_rect(&r1, 0, 0, 10, 10);
  pixman_region_init_rect(&r2, 20, 20, 10, 10);
  pixman_region_init(&ru);
  if (!pixman_region_union(&ru, &r1, &r2))
    fail("pixman_region_union");
  pixman_box16_t *ext = pixman_region_extents(&ru);
  printf("pixman: union extents (%d,%d)-(%d,%d)\n", ext->x1, ext->y1, ext->x2,
         ext->y2);
  if (ext->x1 != 0 || ext->y1 != 0 || ext->x2 != 30 || ext->y2 != 30)
    fail("region union extents wrong");
  pixman_region_fini(&r1);
  pixman_region_fini(&r2);
  pixman_region_fini(&ru);

  printf("[OK] pixman_smoke\n");
  return 0;
}

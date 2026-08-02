/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// test_pixman_smoke — wlroots 前置依赖 pixman 的目标端冒烟测试。
//
// 仅验证纯计算路径：image 创建、region 运算、fill，不触及任何设备或
// wlroots 渲染。证明 build/libpixman-1.so 能被 musl loader 加载、符号可解析、
// 最小 API 行为正确。这是 WF-3（pixman 移植）的运行时验证，与 build.sh
// staging 的产物对应。无 Unity——每步打印标记，任一失败 _exit 非零，
// test_runner 报 [FAIL]。参考 test_egl_smoke.c 的诊断式风格。

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

  // 1. 创建一个 a8r8g8b8 位图 image（8x8），首字节非零说明分配成功。
  pixman_image_t *img =
      pixman_image_create_bits(PIXMAN_a8r8g8b8, 8, 8, NULL, 0);
  if (!img)
    fail("pixman_image_create_bits");
  printf("pixman: created a8r8g8b8 8x8 image %p\n", (void *)img);

  // 2. 用纯色填充整个 image：pixman_image_fill_rectangles。
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
  // 首像素应为红色。PIXMAN_a8r8g8b8 (format 0x00020004) 通道顺序
  // a@24 r@16 g@8 b@0（每通道 8 bit），红色值 0xFFFF0000。按 little-endian
  // uint32 读出 4 字节即该值（最低字节 0x00=b，次 0x00=g，0xff=r，0xff=a）。
  uint32_t *data = (uint32_t *)pixman_image_get_data(img);
  uint32_t px = data[0];
  printf("pixman: filled pixel 0x%08x\n", px);
  if (px != 0xffff0000)
    fail("fill_rectangles produced wrong pixel");
  pixman_image_unref(img);

  // 3. region 运算：两个矩形 region 的并集，检查边界框。
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

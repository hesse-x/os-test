/* Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#include "theme.h"

const struct os_theme_snapshot *os_theme_default(void) {
  static const struct os_theme_snapshot theme = {
      .size = sizeof(theme),
      .version = 1,
      .window_background = 0xff20242a,
      .border_active = 0xff737b86,
      .border_inactive = 0xff4b5159,
      .corner_radius = 9.0f,
      .shadow_radius = 18.0f,
      .titlebar_height = 32.0f,
      .motion_fast_ms = 150,
      .motion_normal_ms = 250,
      .motion_slow_ms = 400,
  };
  return &theme;
}

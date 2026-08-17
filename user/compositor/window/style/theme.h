/*
 * Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#ifndef OS_COMPOSITOR_THEME_H
#define OS_COMPOSITOR_THEME_H

#include <stdint.h>

struct os_theme_snapshot {
  uint32_t size;
  uint32_t version;
  uint32_t window_background;
  uint32_t border_active;
  uint32_t border_inactive;
  float corner_radius;
  float shadow_radius;
  float titlebar_height;
  uint32_t motion_fast_ms;
  uint32_t motion_normal_ms;
  uint32_t motion_slow_ms;
};

const struct os_theme_snapshot *os_theme_default(void);

#endif

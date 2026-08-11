/* Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#ifndef OS_COMPOSITOR_EFFECT_STATE_H
#define OS_COMPOSITOR_EFFECT_STATE_H

#include <stdbool.h>
#include <stdint.h>

struct os_effect_value {
  bool enabled;
  uint32_t radius;
  int32_t x;
  int32_t y;
  int32_t width;
  int32_t height;
};

struct os_effect_state {
  struct os_effect_value pending;
  struct os_effect_value current;
  bool pending_dirty;
  uint64_t generation;
};

void os_effect_state_init(struct os_effect_state *state);
bool os_effect_set_pending(struct os_effect_state *state,
                           const struct os_effect_value *value,
                           uint32_t max_radius);
bool os_effect_apply_surface_commit(struct os_effect_state *state);
void os_effect_discard_pending(struct os_effect_state *state);

#endif

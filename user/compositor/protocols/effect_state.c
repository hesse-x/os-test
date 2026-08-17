/*
 * Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#include "effect_state.h"

#include <limits.h>
#include <string.h>

void os_effect_state_init(struct os_effect_state *state) {
  if (state != NULL) {
    memset(state, 0, sizeof(*state));
    state->generation = 1;
  }
}

bool os_effect_set_pending(struct os_effect_state *state,
                           const struct os_effect_value *value,
                           uint32_t max_radius) {
  if (state == NULL || value == NULL || value->radius > max_radius ||
      value->x < 0 || value->y < 0 || value->width < 0 || value->height < 0 ||
      value->x > INT32_MAX - value->width ||
      value->y > INT32_MAX - value->height)
    return false;
  state->pending = *value;
  state->pending_dirty = true;
  return true;
}

bool os_effect_apply_surface_commit(struct os_effect_state *state) {
  if (state == NULL || !state->pending_dirty)
    return false;
  state->current = state->pending;
  state->pending_dirty = false;
  ++state->generation;
  return true;
}

void os_effect_discard_pending(struct os_effect_state *state) {
  if (state != NULL) {
    memset(&state->pending, 0, sizeof(state->pending));
    state->pending_dirty = false;
  }
}

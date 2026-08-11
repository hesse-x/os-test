/* Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#ifndef OS_COMPOSITOR_WINDOW_STATE_H
#define OS_COMPOSITOR_WINDOW_STATE_H

#include <stdbool.h>
#include <stdint.h>

enum os_window_state {
  OS_WINDOW_VISIBLE,
  OS_WINDOW_MINIMIZING,
  OS_WINDOW_MINIMIZED,
  OS_WINDOW_RESTORING,
  OS_WINDOW_DESTROYING,
};

enum os_window_transition {
  OS_WINDOW_TRANSITION_OK,
  OS_WINDOW_TRANSITION_NOOP,
  OS_WINDOW_TRANSITION_INVALID,
};

struct os_window_state_machine {
  enum os_window_state state;
  uint64_t generation;
};

void os_window_state_init(struct os_window_state_machine *machine);
enum os_window_transition
os_window_request_minimize(struct os_window_state_machine *machine);
enum os_window_transition
os_window_request_restore(struct os_window_state_machine *machine);
enum os_window_transition
os_window_reverse_animation(struct os_window_state_machine *machine);
enum os_window_transition
os_window_finish_animation(struct os_window_state_machine *machine);
enum os_window_transition
os_window_cancel_animation(struct os_window_state_machine *machine);
enum os_window_transition
os_window_begin_destroy(struct os_window_state_machine *machine);
bool os_window_is_visible(const struct os_window_state_machine *machine);
bool os_window_is_animating(const struct os_window_state_machine *machine);

#endif

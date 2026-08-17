/*
 * Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#include "window_state.h"

#include <stddef.h>

static enum os_window_transition
set_state(struct os_window_state_machine *machine, enum os_window_state state) {
  if (machine == NULL)
    return OS_WINDOW_TRANSITION_INVALID;
  if (machine->state == state)
    return OS_WINDOW_TRANSITION_NOOP;
  machine->state = state;
  ++machine->generation;
  return OS_WINDOW_TRANSITION_OK;
}

void os_window_state_init(struct os_window_state_machine *machine) {
  if (machine != NULL) {
    machine->state = OS_WINDOW_VISIBLE;
    machine->generation = 1;
  }
}

enum os_window_transition
os_window_request_minimize(struct os_window_state_machine *machine) {
  if (machine == NULL || machine->state == OS_WINDOW_DESTROYING)
    return OS_WINDOW_TRANSITION_INVALID;
  if (machine->state == OS_WINDOW_VISIBLE)
    return set_state(machine, OS_WINDOW_MINIMIZING);
  if (machine->state == OS_WINDOW_RESTORING)
    return set_state(machine, OS_WINDOW_MINIMIZING);
  return OS_WINDOW_TRANSITION_NOOP;
}

enum os_window_transition
os_window_request_restore(struct os_window_state_machine *machine) {
  if (machine == NULL || machine->state == OS_WINDOW_DESTROYING)
    return OS_WINDOW_TRANSITION_INVALID;
  if (machine->state == OS_WINDOW_MINIMIZED)
    return set_state(machine, OS_WINDOW_RESTORING);
  if (machine->state == OS_WINDOW_MINIMIZING)
    return set_state(machine, OS_WINDOW_RESTORING);
  return OS_WINDOW_TRANSITION_NOOP;
}

enum os_window_transition
os_window_reverse_animation(struct os_window_state_machine *machine) {
  if (machine == NULL)
    return OS_WINDOW_TRANSITION_INVALID;
  if (machine->state == OS_WINDOW_MINIMIZING)
    return set_state(machine, OS_WINDOW_RESTORING);
  if (machine->state == OS_WINDOW_RESTORING)
    return set_state(machine, OS_WINDOW_MINIMIZING);
  return OS_WINDOW_TRANSITION_INVALID;
}

enum os_window_transition
os_window_finish_animation(struct os_window_state_machine *machine) {
  if (machine == NULL)
    return OS_WINDOW_TRANSITION_INVALID;
  if (machine->state == OS_WINDOW_MINIMIZING)
    return set_state(machine, OS_WINDOW_MINIMIZED);
  if (machine->state == OS_WINDOW_RESTORING)
    return set_state(machine, OS_WINDOW_VISIBLE);
  return OS_WINDOW_TRANSITION_INVALID;
}

enum os_window_transition
os_window_cancel_animation(struct os_window_state_machine *machine) {
  if (machine == NULL)
    return OS_WINDOW_TRANSITION_INVALID;
  if (machine->state == OS_WINDOW_MINIMIZING)
    return set_state(machine, OS_WINDOW_VISIBLE);
  if (machine->state == OS_WINDOW_RESTORING)
    return set_state(machine, OS_WINDOW_MINIMIZED);
  return OS_WINDOW_TRANSITION_NOOP;
}

enum os_window_transition
os_window_begin_destroy(struct os_window_state_machine *machine) {
  if (machine == NULL)
    return OS_WINDOW_TRANSITION_INVALID;
  return set_state(machine, OS_WINDOW_DESTROYING);
}

bool os_window_is_visible(const struct os_window_state_machine *machine) {
  return machine != NULL && (machine->state == OS_WINDOW_VISIBLE ||
                             machine->state == OS_WINDOW_RESTORING);
}

bool os_window_is_animating(const struct os_window_state_machine *machine) {
  return machine != NULL && (machine->state == OS_WINDOW_MINIMIZING ||
                             machine->state == OS_WINDOW_RESTORING);
}

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
/* XOS keyboard-driver message; key values use Linux input-event-codes.h. */
#ifndef XOS_KEY_EVENT_H
#define XOS_KEY_EVENT_H

#include <linux/input-event-codes.h>
#include <stdint.h>

#define MOD_SHIFT 0x01
#define MOD_CTRL 0x02
#define MOD_ALT 0x04
#define MOD_CAPS 0x08

struct key_event {
  uint16_t key;
  uint8_t pressed;
  uint8_t modifiers;
};

#endif

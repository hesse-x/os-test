/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef COMMON_INPUT_H
#define COMMON_INPUT_H

// If our libinput compat linux/input.h has already been included, skip
// struct input_event/input_id/input_absinfo redefinition.
#if !defined(COMPAT_LINUX_INPUT_H)

#include <stdint.h>
#include <xos/types.h> // pid_t

// ===================== Input ring buffer protocol (evdev-style)
// =====================

#define INPUT_DEV_KBD 1
#define INPUT_DEV_MOUSE 2
#define INPUT_DEV_TOUCHPAD 3
#define INPUT_DEV_GAMEPAD 4

// evdev-style event types (aligned with linux/input.h)
#define EV_KEY 0x01 // key, button
#define EV_REL 0x02 // relative motion (mouse move, wheel)
#define EV_ABS 0x03 // absolute coord (touchpad, stick)
#define EV_SYN 0x00 // sync separator
#define SYN_REPORT 0
#define SYN_DROPPED 3

// evdev protocol version + capability bounds (aligned with linux/input.h)
#define EV_VERSION 0x010001
#define EV_MAX 0x1f
#define EV_CNT (EV_MAX + 1)
#define KEY_MAX 0x2ff
#define KEY_CNT (KEY_MAX + 1)

// Bus types (subset of linux/input.h)
#define BUS_USB 0x03

/* Standard Linux struct input_event (24 bytes).
 * Layout must match linux/input.h: struct timeval time + __u16 type + __u16
 * code + __s32 value. On x86_64, struct timeval is {__s64 tv_sec; __s64
 * tv_usec} = 16 bytes.
 */
typedef struct input_event {
  int64_t tv_sec;
  int64_t tv_usec;
  uint16_t type; // EV_KEY / EV_REL / EV_ABS / EV_SYN
  uint16_t code; // KEY_A / BTN_LEFT / REL_X / ABS_X ...
  int32_t value; // key: 1=press 0=release; rel: delta; abs: coordinate
} input_event;   // 24 bytes

// Device identity (EVIOCGID)
struct input_id {
  uint16_t bustype;
  uint16_t vendor;
  uint16_t product;
  uint16_t version;
};

// Absolute axis info (EVIOCGABS)
struct input_absinfo {
  int32_t value;
  int32_t minimum;
  int32_t maximum;
  int32_t fuzz;
  int32_t flat;
  int32_t resolution;
};

#endif // !COMPAT_LINUX_INPUT_H

#endif /* COMMON_INPUT_H */

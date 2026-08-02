/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
/* Kernel-side definitions for the Linux evdev ABI. */
#ifndef KERNEL_BSD_INPUT_ABI_H
#define KERNEL_BSD_INPUT_ABI_H

#include <stdint.h>
#include <xos/ioctl.h>
#include <xos/time.h>

#define EV_SYN 0x00
#define EV_KEY 0x01
#define SYN_REPORT 0
#define SYN_DROPPED 3
#define EV_VERSION 0x010001
#define BUS_USB 0x03

typedef struct input_event {
  struct timeval time;
  uint16_t type;
  uint16_t code;
  int32_t value;
} input_event;

struct input_id {
  uint16_t bustype;
  uint16_t vendor;
  uint16_t product;
  uint16_t version;
};

struct input_absinfo {
  int32_t value;
  int32_t minimum;
  int32_t maximum;
  int32_t fuzz;
  int32_t flat;
  int32_t resolution;
};

#define EVIOCGVERSION _IOR('E', 0x01, int)
#define EVIOCGID _IOR('E', 0x02, struct input_id)
#define EVIOCGNAME(len) _IOC(_IOC_READ, 'E', 0x06, len)
#define EVIOCGPROP(len) _IOC(_IOC_READ, 'E', 0x09, len)
#define EVIOCGBIT(ev, len) _IOC(_IOC_READ, 'E', 0x20 + (ev), len)
#define EVIOCGABS(abs) _IOR('E', 0x40 + (abs), struct input_absinfo)
#define EVIOCGRAB _IOW('E', 0x90, int)
#define EVIOCREVOKE _IOW('E', 0x91, int)

_Static_assert(sizeof(struct input_event) == 24,
               "Linux input_event ABI must remain 24 bytes");

#endif

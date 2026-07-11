/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef COMPAT_LINUX_INPUT_H
#define COMPAT_LINUX_INPUT_H

#include <sys/time.h>
#include <sys/types.h>

#include <linux/input-event-codes.h>

// Minimal _IOC macros (same encoding as xos/ioctl.h)
#ifndef _IOC_MACROS
#define _IOC_MACROS
#define _IOC(dir, type, nr, size)                                              \
  (((uint32_t)((dir) << 30) | ((uint32_t)(type) << 8) |                        \
    ((uint32_t)(nr) << 0) | ((uint32_t)(size) << 16)))
#define _IO(type, nr) _IOC(0, type, nr, 0)
#define _IOW(type, nr, sz) _IOC(1, type, nr, sizeof(sz))
#define _IOR(type, nr, sz) _IOC(2, type, nr, sizeof(sz))
#define _IOWR(type, nr, sz) _IOC(3, type, nr, sizeof(sz))
#define _IOC_NONE 0
#define _IOC_WRITE 1
#define _IOC_READ 2
#endif

// Guard struct definitions against xos/input.h when it is included first.
#ifndef COMMON_INPUT_H
struct input_event {
  struct timeval time;
  unsigned short type;
  unsigned short code;
  int value;
};
#define COMMON_INPUT_H

#define input_event_sec time.tv_sec
#define input_event_usec time.tv_usec

struct input_id {
  unsigned short bustype;
  unsigned short vendor;
  unsigned short product;
  unsigned short version;
};

struct input_absinfo {
  int value;
  int minimum;
  int maximum;
  int fuzz;
  int flat;
  int resolution;
};

#define EV_VERSION 0x010001
#else
// xos/input.h already provides struct input_event/input_id/input_absinfo
// and EV_VERSION; keep input_event_sec/usec accessor macros.
#ifndef input_event_sec
#define input_event_sec time.tv_sec
#endif
#ifndef input_event_usec
#define input_event_usec time.tv_usec
#endif
#endif /* !COMMON_INPUT_H */

// Compat-only types (not in xos/input.h); must live outside the
// #ifndef COMMON_INPUT_H guard since EVIOC* macros reference them.
struct input_keymap_entry {
  unsigned char flags;
  unsigned char len;
  unsigned short index;
  unsigned int keycode;
  unsigned int scancode;
};

#define EVIOCGVERSION _IOR('E', 0x01, int)
#define EVIOCGID _IOR('E', 0x02, struct input_id)
#define EVIOCGREP _IOR('E', 0x03, int[2])
#define EVIOCSREP _IOW('E', 0x03, int[2])
#define EVIOCGKEYCODE _IOR('E', 0x04, int[2])
#define EVIOCGKEYCODE_V2 _IOR('E', 0x04, struct input_keymap_entry)
#define EVIOCSKEYCODE _IOW('E', 0x04, int[2])
#define EVIOCSKEYCODE_V2 _IOW('E', 0x04, struct input_keymap_entry)
#define EVIOCGNAME(len) _IOC(_IOC_READ, 'E', 0x06, len)
#define EVIOCGPHYS(len) _IOC(_IOC_READ, 'E', 0x07, len)
#define EVIOCGUNIQ(len) _IOC(_IOC_READ, 'E', 0x08, len)
#define EVIOCGPROP(len) _IOC(_IOC_READ, 'E', 0x09, len)
#define EVIOCGMTSLOTS(len) _IOC(_IOC_READ, 'E', 0x0a, len)
#define EVIOCGKEY(len) _IOC(_IOC_READ, 'E', 0x18, len)
#define EVIOCGLED(len) _IOC(_IOC_READ, 'E', 0x19, len)
#define EVIOCGSND(len) _IOC(_IOC_READ, 'E', 0x1a, len)
#define EVIOCGSW(len) _IOC(_IOC_READ, 'E', 0x1b, len)
#define EVIOCGBIT(ev, len) _IOC(_IOC_READ, 'E', 0x20 + (ev), len)
#define EVIOCGABS(abs) _IOR('E', 0x40 + (abs), struct input_absinfo)
#define EVIOCSABS(abs) _IOW('E', 0x40 + (abs), struct input_absinfo)
#define EVIOCSFF _IOC(_IOC_WRITE, 'E', 0x80, sizeof(struct ff_effect))
#define EVIOCRMFF _IOW('E', 0x81, int)
#define EVIOCGEFFECTS _IOR('E', 0x84, int)
#define EVIOCGRAB _IOW('E', 0x90, int)
#define EVIOCREVOKE _IOW('E', 0x91, int)
#define EVIOCGMASK _IOR('E', 0x92, struct input_mask)
#define EVIOCSMASK _IOW('E', 0x92, struct input_mask)
#define EVIOCSCLOCKID _IOW('E', 0xa0, int)

#endif

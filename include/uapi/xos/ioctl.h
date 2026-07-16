/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef COMMON_IOCTL_H
#define COMMON_IOCTL_H

#include <stdint.h>
#include <xos/input.h> // struct input_id / struct input_absinfo (EVIOCGID/ABS)

// ioctl command encoding (Linux-compatible)
// Shared between kernel and user space.
#ifndef _IOC_MACROS
#define _IOC_MACROS
#define _IOC(dir, type, nr, size)                                              \
  ((uint32_t)(((dir) << 30) | ((type) << 8) | ((nr) << 0) | ((size) << 16)))
#define _IO(type, nr) _IOC(0, type, nr, 0)
#define _IOW(type, nr, sz) _IOC(1, type, nr, sizeof(sz))
#define _IOR(type, nr, sz) _IOC(2, type, nr, sizeof(sz))
#define _IOWR(type, nr, sz) _IOC(3, type, nr, sizeof(sz))
#define _IOC_DIR(cmd) (((cmd) >> 30) & 3)
#define _IOC_TYPE(cmd) (((cmd) >> 8) & 0xFF)
#define _IOC_NR(cmd) ((cmd) & 0xFF)
#define _IOC_SIZE(cmd) (((cmd) >> 16) & 0x3FFF)
#define _IOC_NONE 0
#define _IOC_WRITE 1
#define _IOC_READ 2
#endif /* _IOC_MACROS */

// ===== ioctl command definitions (_IOC-encoded) =====

// Terminal (Linux-compatible, already encoded)
#define TCGETS 0x5401
#define TCSETS 0x5402
#define TCSETSW 0x5403
#define TCSETSF 0x5404
#define TIOCSCTTY 0x540E
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410
#define TIOCGPTN 0x5406
#define TIOCSPTLCK 0x5407
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414

// Input (generic, evdev-style)
#define INPUT_BIND _IOWR('I', 0x01, char[8]) // input_bind_arg = 8B
#define INPUT_UNBIND _IO('I', 0x02)

// evdev query ioctls (type='E', aligned with linux/input.h)
#define EVIOCGVERSION _IOR('E', 0x01, int)
#define EVIOCGID _IOR('E', 0x02, struct input_id)
#define EVIOCGNAME(len) _IOC(_IOC_READ, 'E', 0x06, len)
#define EVIOCGPROP(len) _IOC(_IOC_READ, 'E', 0x09, len)
#define EVIOCGBIT(ev, len) _IOC(_IOC_READ, 'E', 0x20 + (ev), len)
#define EVIOCGABS(abs) _IOR('E', 0x40 + (abs), struct input_absinfo)
#define EVIOCGRAB _IOW('E', 0x90, int)

// HID irqfd bind ioctls (type='H') — bind/unbind an eventfd as the xHCI HID
// interrupt-delivery fd (evdev_refact.md §4.2).  HID_BIND_IRQFD's arg is the
// caller's irqfd fd number (int).
#define HID_BIND_IRQFD _IOW('H', 0x01, int)
#define HID_UNBIND_IRQFD _IO('H', 0x02)

// ringbuf ioctls (type='R')
#define RINGBUF_WAKE _IO('R', 0x01) // wake ringbuf poll/epoll waiters
#define RINGBUF_INJECT                                                         \
  _IOW('R', 0x02, input_event) // inject one event via kernel

#endif

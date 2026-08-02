/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef COMMON_IOCTL_H
#define COMMON_IOCTL_H

#ifndef __KERNEL__
#include <sys/ioctl.h>
#else
#include <stdint.h>

/* The kernel does not consume musl headers, so keep only its private copy of
 * the Linux ioctl encoding. Userspace always gets these from musl. */
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

/* Terminal commands consumed by the kernel. Userspace gets these from musl. */
#define TCGETS 0x5401
#define TCSETS 0x5402
#define TCSETSW 0x5403
#define TCSETSF 0x5404
#define TIOCSCTTY 0x540E
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410
#define TIOCGPTN 0x80045430   /* _IOR('T',0x30,int) — Linux/glibc/musl ABI */
#define TIOCSPTLCK 0x40045431 /* _IOW('T',0x31,int) — Linux/glibc/musl ABI */
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#endif /* __KERNEL__ */

/* musl exposes the Linux encoders but not the decoding helpers from
 * asm-generic/ioctl.h. Keep these as guarded XOS compatibility extensions. */
#ifndef _IOC_DIR
#define _IOC_DIR(cmd) (((cmd) >> 30) & 3)
#define _IOC_TYPE(cmd) (((cmd) >> 8) & 0xFF)
#define _IOC_NR(cmd) ((cmd) & 0xFF)
#define _IOC_SIZE(cmd) (((cmd) >> 16) & 0x3FFF)
#endif

// XOS-specific ioctl command definitions.
// INPUT_BIND / INPUT_UNBIND removed (evdev broker replaces the SHM-ring
// consumer-registration protocol). INPUT_REGISTER (control node) below.
#define INPUT_REGISTER _IOW('I', 0x10, char[68])

// HID irqfd bind ioctls (type='H') — bind/unbind an eventfd as the xHCI HID
// interrupt-delivery fd (evdev_refact.md §4.2).  HID_BIND_IRQFD's arg is the
// caller's irqfd fd number (int).
#define HID_BIND_IRQFD _IOW('H', 0x01, int)
#define HID_UNBIND_IRQFD _IO('H', 0x02)

#endif

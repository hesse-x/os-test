/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef COMPAT_LINUX_INPUT_EVENT_CODES_H
#define COMPAT_LINUX_INPUT_EVENT_CODES_H

// If xos/input.h was already included (reverse include order), it defines
// EV_CNT/KEY_CNT.  Undefine them before including the upstream header to
// avoid -Werror redefinition (the values are identical).
#ifdef COMMON_INPUT_H
#undef EV_CNT
#undef KEY_CNT
#endif

#include <linux/linux/input-event-codes.h>

// BUS_* values — use #ifndef to coexist with xos/input.h when it is included
// first (xos/input.h defines BUS_USB as 0x03; here we use the Linux-standard
// 0x05, but the xos value wins in mixed-include translation units).
#ifndef BUS_ISA
#define BUS_ISA 0x01
#endif
#ifndef BUS_EISA
#define BUS_EISA 0x02
#endif
#ifndef BUS_PCI
#define BUS_PCI 0x03
#endif
#ifndef BUS_AMIGA
#define BUS_AMIGA 0x04
#endif
#ifndef BUS_USB
#define BUS_USB 0x05
#endif
#ifndef BUS_BLUETOOTH
#define BUS_BLUETOOTH 0x07
#endif
#ifndef BUS_I8042
#define BUS_I8042 0x11
#endif
#ifndef BUS_HOST
#define BUS_HOST 0x19
#endif
#ifndef BUS_RMI
#define BUS_RMI 0x1a
#endif
#ifndef BUS_I2C
#define BUS_I2C 0x1c
#endif
#ifndef BUS_SPI
#define BUS_SPI 0x1d
#endif
#ifndef BUS_INTEL_ISHTP
#define BUS_INTEL_ISHTP 0x1e
#endif
#ifndef BUS_AMD_SFH
#define BUS_AMD_SFH 0x1f
#endif

// MT_TOOL values (from linux/input.h, not in libinput's input-event-codes.h)
#define MT_TOOL_FINGER 0
#define MT_TOOL_PEN 1
#define MT_TOOL_PALM 2

#endif

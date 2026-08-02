/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * hidraw UAPI (aligned with <linux/hidraw.h>, type 'H'). /dev/hidraw0 is the
 * raw HID report node for the xHCI keyboard: read() dequeues 8B Boot reports,
 * HIDIOCG* queries device info. Shares the same SHM sub-ring with evdev via
 * mmap (Ring #1, refact_evdev.md sec 14).
 */
#ifndef COMMON_HIDRAW_H
#define COMMON_HIDRAW_H

#include <stdint.h>
#include <xos/ioctl.h> // _IOR/_IOW/_IOC

struct hidraw_devinfo {
  uint32_t bustype;
  int32_t vendor;
  int32_t product;
};

/* Linux input bus type reported through hidraw_devinfo.bustype. */
#define HIDRAW_BUS_USB 0x03

// HIDIOCGRAWINFO: _IOR('H', 0x03, struct hidraw_devinfo)
#define HIDIOCGRAWINFO _IOR('H', 0x03, struct hidraw_devinfo)
#define HIDIOCGRDESCSIZE _IOR('H', 0x01, int)
#define HIDIOCGRDESC _IOR('H', 0x02, char[4096])
#define HIDIOCGFEATURE(len) _IOC(_IOC_READ | _IOC_WRITE, 'H', 0x07, len)
#define HIDIOCSFEATURE(len) _IOC(_IOC_WRITE, 'H', 0x07, len)

#endif /* COMMON_HIDRAW_H */

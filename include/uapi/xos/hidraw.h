/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * hidraw UAPI（对齐 <linux/hidraw.h>，type 'H'）。/dev/hidraw0 是 xHCI 键盘
 * 的裸 HID 报告节点：read() 出队 8B Boot 报告，HIDIOCG* 查询设备信息。
 * 与 evdev 经 mmap 共享同一 SHM 子环（Ring #1，refact_evdev.md §14）。
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

/* HIDIOCGRAWINFO: _IOR('H', 0x03, struct hidraw_devinfo) */
#define HIDIOCGRAWINFO _IOR('H', 0x03, struct hidraw_devinfo)
#define HIDIOCGRDESCSIZE _IOR('H', 0x01, int)
#define HIDIOCGRDESC _IOR('H', 0x02, char[4096])
#define HIDIOCGFEATURE(len) _IOC(_IOC_READ | _IOC_WRITE, 'H', 0x07, len)
#define HIDIOCSFEATURE(len) _IOC(_IOC_WRITE, 'H', 0x07, len)

#endif /* COMMON_HIDRAW_H */

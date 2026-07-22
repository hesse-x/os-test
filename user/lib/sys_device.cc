/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * Hardware-related syscall wrappers: device, IRQ, PCI, dev_ready.
 *
 * Merged from sys_device.cc + sys_irq.cc + sys_pci.cc + dev_ready.c
 */

#include <errno.h>
#include <stdint.h>
#include <syscall.h>
#include <unistd.h>

#include <sys/device.h>
#include <sys/ipc.h>
#include <sys/irq.h>
#include <sys/pci.h>
#include <xos/syscall_asm.h>

#include <xos/errno.h>
#include <xos/fcntl.h>
#include <xos/syscall_nums.h>

// ===================== device registration =====================

extern "C" int device_register(const char *name) {
  return device_register_shm(name, -1, 0);
}

extern "C" int device_register_shm(const char *name, int shm_fd,
                                   uint32_t minor) {
  int r = sys_dev_create(name, shm_fd, minor);
  if (r < 0 && errno != EEXIST)
    return -1;
  return 0;
}

extern "C" int device_set_meta(const char *name, const char *subsystem,
                               const char *devtype,
                               const struct dev_props *props) {
  int64_t r = __syscall4(
      SYS_DEV_SET_META, (int64_t)(uintptr_t)name, (int64_t)(uintptr_t)subsystem,
      (int64_t)(uintptr_t)devtype, (int64_t)(uintptr_t)props);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }
  return 0;
}

// ===================== IRQ =====================

extern "C" int irq_bind(int irq) { return sys_irq_bind(irq); }

// ===================== PCI =====================

extern "C" int pci_dev_info_get(uint8_t bus, uint8_t dev, uint8_t func,
                                struct pci_dev_info *out) {
  return sys_pci_dev_info(bus, dev, func, out);
}

// ===================== wait_dev_ready =====================

extern "C" void wait_dev_ready(const char *dev_path) {
  int fd;
  for (int tries = 0;; tries++) {
    fd = open(dev_path, O_RDWR);
    if (fd >= 0)
      break;
    struct recv_msg m;
    recv(&m, NULL, 0, 10);
  }
  close(fd);
}

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include <sys/device.h>
#include <syscall.h>
#include <xos/errno.h>

int device_register(const char *name) {
  return device_register_shm(name, -1, 0);
}

int device_register_shm(const char *name, int shm_fd, uint32_t minor) {
  int r = sys_dev_create(name, shm_fd, minor);
  if (r < 0 && errno != EEXIST)
    return -1;
  return 0;
}

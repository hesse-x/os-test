/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <sys/mount.h>
#include <syscall.h>

int mount(const char *source, const char *target, const char *fstype,
          unsigned long flags, const void *data) {
  return sys_mount(source, target, fstype, flags, data);
}

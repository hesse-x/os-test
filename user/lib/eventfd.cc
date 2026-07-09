/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// libc eventfd: thin syscall wrapper over SYS_EVENTFD2 (kernel-implemented).
#include <stdint.h>
#include <sys/eventfd.h>
#include <syscall.h>
#include <xos/syscall_asm.h>
#include <xos/syscall_nums.h>

int eventfd(unsigned int initval, int flags) {
  int64_t ret = __syscall2(SYS_EVENTFD2, (int64_t)initval, (int64_t)flags);
  if (ret < 0) {
    errno = (int)(-ret);
    return -1;
  }
  return (int)ret;
}

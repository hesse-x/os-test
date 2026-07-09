/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// libc timerfd: thin syscall wrappers over SYS_TIMERFD_* (kernel-implemented).
#include <stdint.h>
#include <sys/timerfd.h>
#include <syscall.h>
#include <xos/syscall_asm.h>
#include <xos/syscall_nums.h>

int timerfd_create(int clockid, int flags) {
  int64_t ret =
      __syscall2(SYS_TIMERFD_CREATE, (int64_t)clockid, (int64_t)flags);
  if (ret < 0) {
    errno = (int)(-ret);
    return -1;
  }
  return (int)ret;
}

int timerfd_settime(int fd, int flags, const struct itimerspec *new_value,
                    struct itimerspec *old_value) {
  int64_t ret =
      __syscall4(SYS_TIMERFD_SETTIME, (int64_t)fd, (int64_t)flags,
                 (int64_t)(uintptr_t)new_value, (int64_t)(uintptr_t)old_value);
  if (ret < 0) {
    errno = (int)(-ret);
    return -1;
  }
  return (int)ret;
}

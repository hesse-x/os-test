/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// libc signalfd: thin syscall wrapper over SYS_SIGNALFD4 (kernel-implemented).
#include <signal.h>
#include <stdint.h>
#include <sys/signalfd.h>
#include <syscall.h>
#include <xos/syscall_asm.h>
#include <xos/syscall_nums.h>

int signalfd(int fd, const sigset_t *mask, int flags) {
  int64_t ret = __syscall4(SYS_SIGNALFD4, (int64_t)fd, (int64_t)(uintptr_t)mask,
                           (int64_t)sizeof(sigset_t), (int64_t)flags);
  if (ret < 0) {
    errno = (int)(-ret);
    return -1;
  }
  return (int)ret;
}

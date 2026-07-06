/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <syscall.h>

#include <sys/types.h>
#include <sys/wait.h>

pid_t waitpid(pid_t pid, int *status, int options) {
  int64_t r = sys_waitpid(pid, status, options);
  if (r < 0)
    return -1;
  return (pid_t)r;
}

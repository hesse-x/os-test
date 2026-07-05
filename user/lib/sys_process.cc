/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "syscall.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/process.h>
#include <unistd.h>

pid_t fork(void) {
  int64_t r = sys_fork();
  if (r < 0) {
    errno = (int)(-r);
    return -1;
  }
  return (pid_t)r;
}

int execve(const char *pathname, char *const argv[], char *const envp[]) {
  /* When envp is NULL, default to environ (D9 model, inheriting the current
   * environment) */
  return sys_execve(pathname, argv, envp ? envp : environ);
}

pid_t spawn(const char *path) {
  pid_t pid = fork();
  if (pid == 0) {
    execve(path, NULL, NULL);
    _exit(127);
    __builtin_unreachable();
  }
  return pid;
}

pid_t setsid(void) {
  int64_t r = sys_setsid();
  if (r < 0)
    return -1;
  return (pid_t)r;
}

int setpgid(pid_t pid, pid_t pgid) {
  return sys_setpgid((uint64_t)pid, (uint64_t)pgid);
}

pid_t getpgid(pid_t pid) {
  int64_t r = sys_getpgid((uint64_t)pid);
  if (r < 0)
    return -1;
  return (pid_t)r;
}

pid_t getsid(pid_t pid) {
  int64_t r = sys_getsid((uint64_t)pid);
  if (r < 0)
    return -1;
  return (pid_t)r;
}

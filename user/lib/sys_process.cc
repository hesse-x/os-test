/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * Process/memory syscall wrappers: fork/execve/waitpid/mmap/…
 *
 * Merged from sys_process.cc + sys_wait.cc + sys_mman.cc
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/process.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syscall.h>
#include <unistd.h>
#include <xos/mman.h>

extern "C" char **environ;

// ===================== process management =====================

extern "C" pid_t fork(void) {
  int64_t r = sys_fork();
  if (r < 0) {
    errno = (int)(-r);
    return -1;
  }
  return (pid_t)r;
}

extern "C" int execve(const char *pathname, char *const argv[],
                      char *const envp[]) {
  return sys_execve(pathname, argv, envp ? envp : environ);
}

extern "C" pid_t spawn(const char *path) {
  pid_t pid = fork();
  if (pid == 0) {
    execve(path, NULL, NULL);
    _exit(127);
    __builtin_unreachable();
  }
  return pid;
}

extern "C" pid_t setsid(void) {
  int64_t r = sys_setsid();
  if (r < 0)
    return -1;
  return (pid_t)r;
}

extern "C" int setpgid(pid_t pid, pid_t pgid) {
  return sys_setpgid((uint64_t)pid, (uint64_t)pgid);
}

extern "C" pid_t getpgid(pid_t pid) {
  int64_t r = sys_getpgid((uint64_t)pid);
  if (r < 0)
    return -1;
  return (pid_t)r;
}

extern "C" pid_t getsid(pid_t pid) {
  int64_t r = sys_getsid((uint64_t)pid);
  if (r < 0)
    return -1;
  return (pid_t)r;
}

extern "C" int setuid(uid_t uid) { return sys_setuid((uint32_t)uid); }

extern "C" int setgid(gid_t gid) { return sys_setgid((uint32_t)gid); }

extern "C" pid_t waitpid(pid_t pid, int *status, int options) {
  int64_t r = sys_waitpid(pid, status, options);
  if (r < 0)
    return -1;
  return (pid_t)r;
}

// ===================== memory management =====================

extern "C" void *mmap(void *addr, size_t length, int prot, int flags, int fd,
                      uint64_t offset) {
  if (flags & MAP_SHARED) {
    void *r = sys_mmap(addr, length, prot, flags, fd, offset);
    if (r == MAP_FAILED)
      return MAP_FAILED;
    return r;
  }
  void *r = sys_mmap(addr, length, prot, flags, -1, offset);
  if (r == MAP_FAILED)
    return MAP_FAILED;
  return r;
}

extern "C" int munmap(void *addr, size_t length) {
  return sys_munmap(addr, length);
}

extern "C" int memfd_create(const char *name, unsigned int flags) {
  int fd = sys_memfd_create(name, flags);
  if (fd < 0)
    return -1;
  return fd;
}

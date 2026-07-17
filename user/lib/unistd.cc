/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <syscall.h>
#include <unistd.h>

#include <sys/types.h>
#include <xos/confname.h>

pid_t getpid(void) { return (pid_t)sys_getpid(); }

pid_t gettid(void) { return (pid_t)sys_gettid(); }

void _exit(int status) {
  sys_exit(status);
  __builtin_unreachable();
}

int sched_yield(void) {
  sys_yield();
  return 0;
}

int ioperm(unsigned long from, unsigned long num, int turn_on) {
  return sys_ioperm(from, num, turn_on);
}

int ftruncate(int fd, off_t length) { return sys_ftruncate(fd, length); }

// --- POSIX identity & permissions (group 1) ---
uid_t getuid(void) { return (uid_t)sys_getuid(); }
uid_t geteuid(void) { return (uid_t)sys_geteuid(); }
gid_t getgid(void) { return (gid_t)sys_getgid(); }
gid_t getegid(void) { return (gid_t)sys_getegid(); }
pid_t getppid(void) { return (pid_t)sys_getppid(); }
pid_t getpgrp(void) { return (pid_t)sys_getpgrp(); }
mode_t umask(mode_t mask) { return (mode_t)sys_umask((int)mask); }

int gethostname(char *name, size_t len) { return sys_gethostname(name, len); }

int sethostname(const char *name, size_t len) {
  return sys_sethostname(name, len);
}

// --- alarm / pause (group 2) ---
unsigned int alarm(unsigned int seconds) {
  return (unsigned int)sys_alarm(seconds);
}

int pause(void) { return sys_pause(); }

// --- truncate / fsync / sync (group 3) ---
int truncate(const char *path, off_t length) {
  return sys_truncate(path, (int64_t)length);
}

int fsync(int fd) { return sys_fsync(fd); }

void sync(void) { sys_sync(); }

int getpagesize(void) { return 4096; }

long sysconf(int name) {
  // Dynamic values are backed by sys_sysconf (ncpu, total/free phys pages).
  // Static/architecture-fixed values (PAGESIZE, CLK_TCK, OPEN_MAX, …) stay
  // here — glibc hardcodes them too. Unknown → -1, errno unchanged (POSIX).
  switch (name) {
  case _SC_NPROCESSORS_CONF:
  case _SC_NPROCESSORS_ONLN:
  case _SC_PHYS_PAGES:
  case _SC_AVPHYS_PAGES:
    return sys_sysconf(name);
  case _SC_PAGESIZE: // _SC_PAGE_SIZE is the same value (30)
    return 4096;
  case _SC_CLK_TCK:
    return 100;
  case _SC_OPEN_MAX:
    return 128; // MAX_FD (kernel/bsd/types.h)
  default:
    return -1;
  }
}

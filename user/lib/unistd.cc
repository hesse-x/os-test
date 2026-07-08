/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <sys/types.h>
#include <syscall.h>
#include <unistd.h>

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

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * Process/memory syscall wrappers: fork/execve/waitpid/mmap/…
 *
 * Merged from sys_process.cc + sys_wait.cc + sys_mman.cc
 */

#include <errno.h>
#include <stdint.h>
#include <syscall.h>
#include <unistd.h> // IWYU pragma: keep

#include <sys/mman.h>
#include <sys/process.h>
#include <sys/wait.h>
#include <xos/unistd_ext.h>

extern "C" char **environ;

// ===================== process management =====================

/* gettid: thread ID of the calling thread (Linux gettid(2)). musl has no
 * public declaration, so this OS exposes it via <xos/unistd_ext.h>; the
 * LIBC_EXPORT there gives the definition C linkage + default visibility. */
extern "C" pid_t gettid(void) { return (pid_t)sys_gettid(); }

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

/* setsid/setpgid/getpgid/getsid/setuid/setgid/setresuid/setresgid/setreuid/
 * setregid/seteuid/setegid/getgroups are provided by musl src/unistd
 * (musl_unistd_objs). The set*id variants call __setxid, supplied by
 * lib/musl_shim/syscall_cp.c as a plain __syscall3 (process-wide, no
 * __synccall broadcast) — identical to the kernel's process-wide creds and
 * to this repo's former direct-syscall wrappers. errno mapping is musl's
 * __syscall_ret. The saved-set / permission-ladder semantics exercised by
 * test_setxid.c are in the kernel's sys_setresuid etc., unchanged. */

extern "C" pid_t waitpid(pid_t pid, int *status, int options) {
  int64_t r = sys_waitpid(pid, status, options);
  if (r < 0)
    return -1;
  return (pid_t)r;
}

// ===================== memory management =====================
//
// mmap/munmap/mprotect/mremap are provided by musl upstream (musl_mman_objs,
// src/mman/*.c) — deleted from here when the mman module switched to musl.
// musl's wrappers route through the same SYS_mmap(9)/SYS_munmap(11)/
// SYS_mprotect(10)/SYS_mremap(25) the old hand-written ones used (mmap.c
// additionally fixes EPERM→ENOMEM and validates offset alignment).
// memfd_create is RETAINED here: musl src/mman has no memfd_create (it lives
// in musl src/linux/), and pulling that in would drag the whole src/linux glob.

extern "C" int memfd_create(const char *name, unsigned int flags) {
  int fd = sys_memfd_create(name, flags);
  if (fd < 0)
    return -1;
  return fd;
}

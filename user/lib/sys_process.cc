/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * Process/memory syscall wrappers: fork/spawn/mmap/…
 *
 * Merged from sys_process.cc + sys_wait.cc + sys_mman.cc
 */

#include <errno.h>
#include <stdint.h>
#include <unistd.h> // IWYU pragma: keep
#include <xos/syscall_ext.h>

#include <sys/process.h>

// ===================== process management =====================

/* gettid now comes from musl src/linux/gettid.c (musl_linux_objs): musl's
 * version returns the cached __pthread_self()->tid set by __init_tls via
 * SYS_set_tid_address (every ELF runs __init_tls at startup), equivalent to
 * the former per-call sys_gettid() here. It is declared in musl's <unistd.h>
 * under _GNU_SOURCE. */

extern "C" pid_t fork(void) {
  int64_t r = sys_fork();
  if (r < 0) {
    errno = (int)(-r);
    return -1;
  }
  return (pid_t)r;
}

/* execve/wait/waitpid/waitid (and the execl/execle/execlp/execv/execvp/fexecve
 * varargs wrappers) now come from musl src/process (musl_process_objs). musl's
 * execve is `syscall(SYS_execve)` — the kernel tolerates envp==NULL
 * (kernel/bsd/proc.c:1718 `if (envp_ptr)`), so it matches the former hand-
 * written `envp ? envp : environ` fallback. waitpid routes through
 * sys_wait4_cp (a cancellation point, per POSIX) instead of the former bare
 * sys_waitpid. Kept here: fork (bare sys_fork; musl's fork pulls the atfork
 * lock table + TLS reset, out of scope) and spawn (private, non-POSIX). */

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

// ===================== memory management =====================
//
// mmap/munmap/mprotect/mremap are provided by musl upstream (musl_mman_objs,
// src/mman/*.c) — deleted from here when the mman module switched to musl.
// musl's wrappers route through the same SYS_mmap(9)/SYS_munmap(11)/
// SYS_mprotect(10)/SYS_mremap(25) the old hand-written ones used (mmap.c
// additionally fixes EPERM→ENOMEM and validates offset alignment).
// memfd_create now comes from musl src/linux/memfd_create.c (musl_linux_objs,
// routes to SYS_memfd_create). The musl mman module does NOT compile it
// (mman.cmake:27) — it lives in src/linux, now pulled in by this batch.

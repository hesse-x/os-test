/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// libc wrappers for flock(2) and accept4(2). musl ships src/linux/flock.c and
// src/network/accept4.c, but only the unistd/fcntl/pthread musl source subsets
// are compiled into libc.so, so these two would otherwise be absent. Provide
// thin shims over the kernel SYS_FLOCK / SYS_ACCEPT4 syscalls. The public
// prototypes come from <sys/file.h> (musl — flock) and <sys/socket.h> (our
// shim — accept4); both declare C linkage, so these definitions inherit it
// (same pattern as signal.cc's kill).
#define _DEFAULT_SOURCE

#include <errno.h>
#include <stdint.h>

#include <sys/cdefs.h>  // LIBC_EXPORT
#include <sys/file.h>   // flock prototype + LOCK_*
#include <sys/socket.h> // accept4 prototype + sockaddr/socklen_t
#include <xos/syscall_asm.h>
#include <xos/syscall_nums.h>

LIBC_EXPORT int flock(int fd, int operation) {
  int64_t r = __syscall2(SYS_FLOCK, (int64_t)fd, (int64_t)operation);
  if (r < 0) {
    errno = (int)(-r);
    return -1;
  }
  return (int)r;
}

LIBC_EXPORT int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen,
                        int flags) {
  int64_t r = __syscall4(SYS_ACCEPT4, (int64_t)sockfd, (int64_t)(uintptr_t)addr,
                         (int64_t)(uintptr_t)addrlen, (int64_t)flags);
  if (r < 0) {
    errno = (int)(-r);
    return -1;
  }
  return (int)r;
}

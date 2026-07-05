/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _FCNTL_H
#define _FCNTL_H

#include <stdint.h>
#include <sys/cdefs.h>
#include <xos/fcntl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* POSIX FD_CLOEXEC (userspace convention = 1). The kernel uses bit 0x8000
 * internally; the two do not interfere. This OS has no exec, so
 * F_SETFD/FD_CLOEXEC are placeholders (see file.cc fcntl downgrade). */
#define FD_CLOEXEC 1

LIBC_EXPORT int open(const char *path, int flags, ...);
LIBC_EXPORT int dup2(int old_fd, int new_fd);
LIBC_EXPORT int fcntl(int fd, int cmd, ...);
LIBC_EXPORT uint64_t fd_file_size(int fd);

#ifdef __cplusplus
}
#endif

#endif /* _FCNTL_H */

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _SYS_STATFS_H
#define _SYS_STATFS_H

#include <stddef.h>
#include <sys/cdefs.h>
#include <xos/statfs.h> /* struct statfs — shared kernel+user ABI */

#ifdef __cplusplus
extern "C" {
#endif

/* statfs/fstatfs — filesystem statistics.  Thin syscall wrappers; the
 * struct layout and FS magic constants live in xos/statfs.h (single source
 * of truth shared with the kernel). */
LIBC_EXPORT int statfs(const char *path, struct statfs *buf);
LIBC_EXPORT int fstatfs(int fd, struct statfs *buf);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_STATFS_H */

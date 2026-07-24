/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _SYS_MOUNT_H
#define _SYS_MOUNT_H

#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

LIBC_EXPORT int mount(const char *source, const char *target,
                      const char *fstype, unsigned long flags,
                      const void *data);

/* mount(2) flags — Linux x86-64 values (uapi linux/fs.h). MS_RDONLY/NOSUID/
 * NODEV/NOEXEC are accepted as no-op (no permission/exec-bit semantics);
 * MS_REMOUNT/MS_BIND are rejected with ENOSYS (not implemented). */
#define MS_RDONLY 0x00000001
#define MS_NOSUID 0x00000002
#define MS_NODEV 0x00000004
#define MS_NOEXEC 0x00000008
#define MS_BIND 0x00001000
#define MS_REMOUNT 0x80000000

#ifdef __cplusplus
}
#endif

#endif /* _SYS_MOUNT_H */

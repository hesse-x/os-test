/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _SYS_PROCESS_H
#define _SYS_PROCESS_H

#include <stddef.h>
#include <sys/cdefs.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

LIBC_EXPORT pid_t fork(void);
LIBC_EXPORT int execve(const char *pathname, char *const argv[],
                       char *const envp[]);
LIBC_EXPORT pid_t spawn(const char *path);
LIBC_EXPORT pid_t setsid(void);
LIBC_EXPORT int setpgid(pid_t pid, pid_t pgid);
LIBC_EXPORT pid_t getpgid(pid_t pid);
LIBC_EXPORT pid_t getsid(pid_t pid);
LIBC_EXPORT int setuid(uid_t uid);
LIBC_EXPORT int setgid(gid_t gid);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_PROCESS_H */

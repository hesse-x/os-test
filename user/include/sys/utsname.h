/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _SYS_UTSNAME_H
#define _SYS_UTSNAME_H

#define UTSNAME_LEN 65

#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

struct utsname {
  char sysname[UTSNAME_LEN];
  char nodename[UTSNAME_LEN];
  char release[UTSNAME_LEN];
  char version[UTSNAME_LEN];
  char machine[UTSNAME_LEN];
};

LIBC_EXPORT int uname(struct utsname *buf);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_UTSNAME_H */

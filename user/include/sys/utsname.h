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

// 6-field layout, matching musl's <sys/utsname.h> and the kernel UAPI
// struct new_utsname (include/uapi/xos/utsname.h): sysname, nodename, release,
// version, machine, domainname — each UTSNAME_LEN (65) bytes = 390 bytes total.
// The kernel's sys_uname (kernel/bsd/syscall.c) copies exactly this 390-byte
// struct to user space, so the libc struct MUST be 390 bytes or the kernel
// overruns the caller's buffer. (The old 5-field 325-byte struct here was a
// latent ABI bug, masked because the repo's uname.c hard-coded strings and
// never called the syscall.) domainname is exposed under _GNU_SOURCE per glibc/
// musl convention; the 6th field always occupies the same storage regardless.
struct utsname {
  char sysname[UTSNAME_LEN];
  char nodename[UTSNAME_LEN];
  char release[UTSNAME_LEN];
  char version[UTSNAME_LEN];
  char machine[UTSNAME_LEN];
#ifdef _GNU_SOURCE
  char domainname[UTSNAME_LEN];
#else
  char __domainname[UTSNAME_LEN];
#endif
};

// Size invariant: must equal the kernel new_utsname (6 * 65 = 390). Catches any
// future field/length drift between this header and the UAPI at compile time.
_Static_assert(sizeof(struct utsname) == 6 * UTSNAME_LEN,
               "struct utsname must match kernel new_utsname (xos/utsname.h)");

int uname(struct utsname *buf);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_UTSNAME_H */

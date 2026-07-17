/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * Shared <unistd.h> sysconf() configuration-name constants.
 *
 * glibc defines these as an enum in <bits/confname.h>; we mirror glibc's
 * integer values exactly (see plan §2.3) so that glibc-built objects stay
 * interoperable and so the kernel-side sys_sysconf switch compares against
 * the same literals the user-side sysconf compiles with. Kept in uapi so the
 * kernel (sys_sysconf) and user libc (sysconf) share one definition.
 */
#ifndef UAPI_XOS_CONFNAME_H
#define UAPI_XOS_CONFNAME_H

#define _SC_ARG_MAX 0
#define _SC_CHILD_MAX 1
#define _SC_CLK_TCK 2
#define _SC_NGROUPS_MAX 3
#define _SC_OPEN_MAX 4
#define _SC_STREAM_MAX 5
#define _SC_TZNAME_MAX 6
#define _SC_PAGESIZE 30
#define _SC_PAGE_SIZE 30 /* glibc synonym */
#define _SC_NPROCESSORS_CONF 83
#define _SC_NPROCESSORS_ONLN 84
#define _SC_PHYS_PAGES 85
#define _SC_AVPHYS_PAGES 86

#endif /* UAPI_XOS_CONFNAME_H */

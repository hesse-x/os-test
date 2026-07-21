/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef COMMON_UTSNAME_H
#define COMMON_UTSNAME_H

// Linux UAPI: struct new_utsname (6 fields × 65 chars)
// Aligned to linux/include/uapi/linux/utsname.h
#define __NEW_UTS_LEN 65

typedef struct new_utsname {
  char sysname[__NEW_UTS_LEN];    /* OS name */
  char nodename[__NEW_UTS_LEN];   /* hostname */
  char release[__NEW_UTS_LEN];    /* OS release */
  char version[__NEW_UTS_LEN];    /* OS version */
  char machine[__NEW_UTS_LEN];    /* hardware identifier */
  char domainname[__NEW_UTS_LEN]; /* NIS domain name */
} new_utsname;

#endif /* COMMON_UTSNAME_H */

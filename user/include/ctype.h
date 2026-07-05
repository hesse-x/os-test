/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _CTYPE_H
#define _CTYPE_H

#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

LIBC_EXPORT int isdigit(int c);
LIBC_EXPORT int isalpha(int c);
LIBC_EXPORT int isalnum(int c);
LIBC_EXPORT int isprint(int c);
LIBC_EXPORT int isspace(int c);
LIBC_EXPORT int ispunct(int c);
LIBC_EXPORT int islower(int c);
LIBC_EXPORT int isupper(int c);
LIBC_EXPORT int isblank(int c);
LIBC_EXPORT int iscntrl(int c);
LIBC_EXPORT int isxdigit(int c);
LIBC_EXPORT int isgraph(int c);
LIBC_EXPORT int isascii(int c);
LIBC_EXPORT int tolower(int c);
LIBC_EXPORT int toupper(int c);

#ifdef __cplusplus
}
#endif

#endif /* _CTYPE_H */

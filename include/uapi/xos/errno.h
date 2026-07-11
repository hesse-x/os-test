/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef COMMON_ERRNO_H
#define COMMON_ERRNO_H

#define EOK 0
#define EPERM 1
#define ENOENT 2
#define ENOMEM 3
#define EINVAL 4
#define ENOSYS 5
#define ECHILD 6
#define EFAULT 7
#define EEXIST 8
#define EBUSY 9
#define ESRCH 10
#define ETIMEDOUT 11
#define EBADF 12
#define EMFILE 13
#define EISDIR 14
#define ENOTDIR 15
#define EIO 16
#define ENOSPC 17
#define EAGAIN 18
#define EPIPE 19
#define EMSGSIZE 20
#define EAFNOSUPPORT 21
#define EPROTONOSUPPORT 22
#define ECONNREFUSED 23
#define ENOTCONN 24
#define EADDRINUSE 25
#define ENOTSOCK 26
#define ENOTSUP 27
#define ENXIO 28
#define ESPIPE 29
#define EROFS 30
#define EMLINK 31
#define EDOM 32
#define ERANGE 33
#define ENOTEMPTY 34
#define EDEADLK 35
#define ENODEV 36
#define ENOTTY 37
#define EINTR 38
#define ENOEXEC 39
#define ELOOP 40
#define ENAMETOOLONG 41
#define ESOCKTNOSUPPORT 44

/* POSIX errno values that do not collide with existing assignments use the
 * Linux numbers for cross-ecology familiarity. Two Linux values collide with
 * existing slots (EACCES=13 vs EMFILE=13; ENFILE=23 vs ECONNREFUSED=23), so
 * they are pushed to free high numbers (100/101). See doc/design/kernel/
 * posix.md "errno conventions". A full Linux-aligned renumber is a separate
 * item. */
#define ENOMSG 42
#define EIDRM 43
#define ENOLCK 46
#define EBADE 52
#define ENOSTR 60
#define ENODATA 61
#define ETIME 62
#define ENOSR 63
#define ENOLINK 67
#define EPROTO 71
#define EMULTIHOP 72
#define EBADMSG 74
#define EOVERFLOW 75
#define EOPNOTSUPP 95
#define EACCES 100
#define ENFILE 101

#define ECONNRESET 104
#define EWOULDBLOCK EAGAIN

#endif // COMMON_ERRNO_H

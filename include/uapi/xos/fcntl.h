/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _COMMON_FCNTL_H
#define _COMMON_FCNTL_H

// Open flags (Linux x86-64 octal values)
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0100
#define O_EXCL 0200
#define O_NOCTTY 0400
#define O_TRUNC 01000
#define O_APPEND 02000
#define O_NONBLOCK 04000
#define O_DSYNC                                                                \
  04000 /* same as O_NONBLOCK on x86-64; Linux uses O_DSYNC=010000 separately  \
         */
#define __O_SYNC 04000000
#define O_SYNC (__O_SYNC | O_DSYNC)
#define O_CLOEXEC 020000000
#define O_DIRECT 040000
#define O_DIRECTORY 0200000
#define O_NOFOLLOW 04000000
#define O_PATH 0100000000
#define O_TMPFILE 02020000000
#define O_LARGEFILE 0

#define O_SETFL_MASK (O_NONBLOCK | O_APPEND)

// fcntl commands (Linux x86-64 values)
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_GETLK 5
#define F_SETLK 6
#define F_SETLKW 7
#define F_SETOWN 8
#define F_GETOWN 9
#define F_SETSIG 10
#define F_GETSIG 11
#define F_GETPIPE_SZ 31
#define F_SETPIPE_SZ 32
#define F_DUPFD_CLOEXEC 1030

/* Note: POSIX FD_CLOEXEC=1 is not defined here. The kernel internally uses
 * FD_CLOEXEC=0x8000 in kernel/bsd/types.h as the fd flags bit (separate from
 * O_*). The userspace FD_CLOEXEC is defined in user/include/fcntl.h (=1, POSIX
 * convention). */

// Linux-compatible sealing constants (for memfd_create + fcntl)
#define F_ADD_SEALS 1033
#define F_GET_SEALS 1034
#define F_SEAL_SEAL 0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW 0x0004
#define F_SEAL_WRITE 0x0008

// *at() syscall constants
#define AT_FDCWD -100
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR 0x200
#define AT_EACCESS 0x200 /* same value as AT_REMOVEDIR on Linux x86-64 */

#endif /* _COMMON_FCNTL_H */

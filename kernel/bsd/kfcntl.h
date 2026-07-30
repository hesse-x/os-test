/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * Kernel-private fcntl constants and struct layouts. This was the shared UAPI
 * header include/uapi/xos/fcntl.h; during the musl fcntl adoption
 * (fcntl_worklist §3b) it moved here so the userspace libc could switch to
 * musl's real <fcntl.h> without O_*, F_*, struct flock colliding. The userspace
 * <fcntl.h> is now a shim to musl; cross-consistency between this header and
 * musl's bits/fcntl.h is locked at compile time by kernel/bsd/fcntl_sync.c
 * (fcntl_worklist §3c). NOT published to the sysroot.
 */

#ifndef _KERNEL_BSD_KFCNTL_H
#define _KERNEL_BSD_KFCNTL_H

#include <stddef.h>
#include <stdint.h>

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
// Linux x86-64 octal values (aligned with musl arch/x86_64/bits/fcntl.h).
#define O_DSYNC 010000
#define __O_SYNC 04000000
#define O_SYNC (__O_SYNC | O_DSYNC) /* = 04010000 */
#define O_CLOEXEC 02000000
#define O_DIRECT 040000
#define O_DIRECTORY 0200000
#define O_NOFOLLOW 0400000
#define O_PATH 010000000
#define O_TMPFILE 020200000 /* __O_TMPFILE(020000000) | O_DIRECTORY */
// musl 1.2.x dropped arch/x86_64/bits/fcntl.h and falls through to
// arch/generic/bits/fcntl.h, which defines O_LARGEFILE = 0100000 (the Linux
// UAPI asm-generic value). On 64-bit it is a no-op (off_t is already 64-bit),
// so the kernel ignores the bit; the value must still match musl so the
// fcntl_sync.c ABI guard passes and userspace's O_LARGEFILE round-trips
// cleanly through open(). v1.1.19's x86-64 bits/fcntl.h had 0 here.
#define O_LARGEFILE 0100000

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
#define F_SETOWN_EX 15
#define F_GETOWN_EX 16
#define F_GETPIPE_SZ 1032
#define F_SETPIPE_SZ 1031
#define F_DUPFD_CLOEXEC 1030

/* POSIX record lock types (struct flock.l_type). */
#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2

/* OFD (open file description) locks — Linux 3.15+. Owned by the open file
 * description (struct file*) rather than the process: two independent open()s
 * of the same file conflict (unlike POSIX), while dup()'d fds sharing one
 * description do not. Implemented in kernel/bsd/file_lock.c. */
#define F_OFD_GETLK 36
#define F_OFD_SETLK 37
#define F_OFD_SETLKW 38

/* f_owner_ex.type — F_SETOWN_EX/F_GETOWN_EX recipient class. This OS stores
 * the value (no SIGIO delivery path) so F_GETOWN_EX round-trips it verbatim. */
#define F_OWNER_TID 0
#define F_OWNER_PID 1
#define F_OWNER_PGRP 2

/* Maximum pipe capacity for F_SETPIPE_SZ (Linux PIPE_MAX_SIZE). */
#define PIPE_MAX_SIZE (1 << 20)

/* POSIX record lock descriptor (Linux x86-64 struct flock layout):
 *   l_type(2) l_whence(2) pad(4) l_start(8) l_len(8) l_pid(4) pad(4) = 32
 * bytes. long is 8 bytes on x86-64 (8-byte aligned → 4 bytes padding after
 * l_whence); l_pid is a 4-byte pid_t (int32_t) with 4 bytes trailing padding.
 * int32_t is used (not pid_t) to keep this uapi header from needing
 * <xos/types.h>. */
struct flock {
  short l_type;
  short l_whence;
  long l_start;
  long l_len;
  int32_t l_pid;
};

/* F_SETOWN_EX / F_GETOWN_EX recipient descriptor (Linux x86-64 layout):
 *   type(4) pid(4) = 8 bytes. int32_t (not pid_t) keeps this uapi header free
 *   of <xos/types.h>; matches struct flock's l_pid convention. */
struct f_owner_ex {
  int32_t type;
  int32_t pid;
};

#ifdef __cplusplus
static_assert(offsetof(struct flock, l_type) == 0, "flock l_type");
static_assert(offsetof(struct flock, l_start) == 8, "flock l_start");
static_assert(sizeof(struct flock) == 32, "flock size (x86-64)");
#else
_Static_assert(offsetof(struct flock, l_type) == 0, "flock l_type");
_Static_assert(offsetof(struct flock, l_start) == 8, "flock l_start");
_Static_assert(sizeof(struct flock) == 32, "flock size (x86-64)");
#endif

/* Note: POSIX FD_CLOEXEC=1 is not defined here. The kernel internally uses
 * FD_CLOEXEC=0x8000 in kernel/bsd/types.h as the fd flags bit (separate from
 * O_*). The userspace FD_CLOEXEC=1 comes from musl's <fcntl.h> (POSIX
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
#define AT_EMPTY_PATH                                                          \
  0x1000 /* S07: fstatat/openat operate on dirfd itself when path=="" */
#define AT_NO_AUTOMOUNT 0x800
#define AT_SYMLINK_FOLLOW                                                      \
  0x400 /* linkat: follow symlink at linkpath creation */

/* access(2)/faccessat(2) mode bits. Shared kernel+user uapi so the kernel
 * inode_permission() and the user-side access() agree on the same literals. */
#define F_OK 0 /* file exists */
#define R_OK 4 /* test for read permission */
#define W_OK 2 /* test for write permission */
#define X_OK 1 /* test for execute (search) permission */

/* utimensat times[] special tv_nsec values (Linux uapi). Used by sys_utimensat
 * (do_utimensat in kernel/bsd/syscall.c) to set atime/mtime to now or leave
 * them unchanged. These MUST match Linux/musl exactly: musl's <sys/stat.h>
 * defines UTIME_NOW=0x3fffffff, UTIME_OMIT=0x3ffffffe. The old shared
 * xos/fcntl.h had the two swapped (UTIME_NOW=(1<<30)-2, UTIME_OMIT=(1<<30)-1);
 * masked because kernel+userspace shared the same wrong values. The split
 * exposed it — fixed here, and locked by kernel/bsd/fcntl_sync.c. */
#define UTIME_NOW 0x3fffffff  /* (1U<<30)-1 */
#define UTIME_OMIT 0x3ffffffe /* (1U<<30)-2 */

#endif /* _KERNEL_BSD_KFCNTL_H */

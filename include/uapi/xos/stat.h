#ifndef _COMMON_STAT_H
#define _COMMON_STAT_H

#include <stdint.h>

/*
 * Kernel-internal stat layout — must match userspace struct stat
 * (user/include/sys/stat.h) exactly.  Both kernel and libc include
 * this header so the layout can never silently diverge.
 *
 * Linux x86-64 ABI (natural alignment, 144 bytes):
 *   offset   0: st_dev      uint64_t
 *   offset   8: st_ino      uint64_t
 *   offset  16: st_nlink    uint64_t
 *   offset  24: st_mode     uint32_t
 *   offset  28: st_uid      uint32_t
 *   offset  32: st_gid      uint32_t
 *   offset  36: __pad0      uint32_t
 *   offset  40: st_rdev     uint64_t
 *   offset  48: st_size     int64_t
 *   offset  56: st_blksize  int64_t
 *   offset  64: st_blocks   int64_t
 *   offset  72: st_atim     timespec {int64_t, int64_t}
 *   offset  88: st_mtim     timespec
 *   offset 104: st_ctim     timespec
 *   offset 120: __reserved  int64_t[3]
 *   Total: 144 bytes
 */

struct kstat_timespec {
  int64_t tv_sec;
  int64_t tv_nsec;
};

struct kstat {
  uint64_t st_dev;
  uint64_t st_ino;
  uint64_t st_nlink;
  uint32_t st_mode;
  uint32_t st_uid;
  uint32_t st_gid;
  uint32_t __pad0;
  uint64_t st_rdev;
  int64_t  st_size;
  int64_t  st_blksize;
  int64_t  st_blocks;
  struct kstat_timespec st_atim;
  struct kstat_timespec st_mtim;
  struct kstat_timespec st_ctim;
  int64_t __reserved[3];
};

// File type constants (shared between kernel and userspace)
#define S_IFMT 0170000
#define S_IFSOCK 0140000
#define S_IFLNK 0120000
#define S_IFREG 0100000
#define S_IFBLK 0060000
#define S_IFDIR 0040000
#define S_IFCHR 0020000
#define S_IFIFO 0010000

// Set-user-ID / set-group-ID / sticky bit
#define S_ISUID 0004000
#define S_ISGID 0002000
#define S_ISVTX 0001000

// Permission bits
#define S_IRWXU 00700
#define S_IRUSR 00400
#define S_IWUSR 00200
#define S_IXUSR 00100
#define S_IRWXG 00070
#define S_IRGRP 00040
#define S_IWGRP 00020
#define S_IXGRP 00010
#define S_IRWXO 00007
#define S_IROTH 00004
#define S_IWOTH 00002
#define S_IXOTH 00001

// File type tests
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m) (((m) & S_IFMT) == S_IFBLK)

#endif

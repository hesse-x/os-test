#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <stddef.h>
#include <sys/cdefs.h>
#include <sys/types.h>
#include <time.h>
#include <xos/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Userspace struct stat — mirrors struct kstat from xos/stat.h.
 * Kept as a separate type so user code uses standard POSIX names. */
struct stat {
  dev_t st_dev;
  ino_t st_ino;
  mode_t st_mode;
  nlink_t st_nlink;
  uid_t st_uid;
  gid_t st_gid;
  off_t st_size;
  blksize_t st_blksize;
  blkcnt_t st_blocks;
  struct timespec st_atim;
  struct timespec st_mtim;
  struct timespec st_ctim;
};

/* Compile-time assertion: struct stat and struct kstat must have identical
 * layout. We check key offsets that previously diverged (st_size). */
#if defined(__cplusplus)
static_assert(offsetof(struct stat, st_size) == offsetof(struct kstat, st_size),
              "struct stat and struct kstat st_size offset mismatch");
#else
_Static_assert(offsetof(struct stat, st_size) ==
                   offsetof(struct kstat, st_size),
               "struct stat and struct kstat st_size offset mismatch");
#endif

/* File type constants (S_IFMT..S_IFIFO), permission bits (S_ISUID..S_IXOTH),
 * and type tests (S_ISREG/S_ISDIR/S_ISCHR/S_ISBLK) come from xos/stat.h
 * (included above) — single source of truth shared with the kernel. */

LIBC_EXPORT int stat(const char *path, struct stat *st);
LIBC_EXPORT int fstat(int fd, struct stat *st);
LIBC_EXPORT int mkdir(const char *path, mode_t mode);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_STAT_H */

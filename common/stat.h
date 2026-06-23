#ifndef _COMMON_STAT_H
#define _COMMON_STAT_H

#include <stdint.h>

/*
 * Kernel-internal stat layout — must match userspace struct stat
 * (user/include/sys/stat.h) exactly.  Both kernel and libc include
 * this header so the layout can never silently diverge.
 *
 * x86-64 layout (natural alignment):
 *   offset  0: st_dev      uint32_t
 *   offset  4: st_ino      uint32_t
 *   offset  8: st_mode     uint32_t
 *   offset 12: st_nlink    uint32_t
 *   offset 16: st_uid      uint32_t
 *   offset 20: st_gid      uint32_t
 *   offset 24: st_size     uint64_t  (8-byte aligned)
 *   offset 32: st_blksize  int32_t
 *   offset 36: st_blocks   int32_t
 *   offset 40: st_atim     timespec  {long, long}
 *   offset 56: st_mtim     timespec
 *   offset 72: st_ctim     timespec
 */

struct kstat_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct kstat {
    uint32_t st_dev;
    uint32_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_size;
    int32_t  st_blksize;
    int32_t  st_blocks;
    struct kstat_timespec st_atim;
    struct kstat_timespec st_mtim;
    struct kstat_timespec st_ctim;
};

#endif

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_BSD_STATX_ABI_H
#define KERNEL_BSD_STATX_ABI_H

#include <stdint.h>

// statx ABI — must match Linux x86-64 exactly (uapi linux/stat.h).
// Kernel-side mirror of the Linux ABI. Userspace gets this definition from
// musl's <sys/stat.h>. Total size: 256 bytes.
//
//   offset   0: stx_mask             uint32_t
//   offset   4: stx_blksize          uint32_t
//   offset   8: stx_attributes       uint64_t
//   offset  16: stx_nlink            uint32_t
//   offset  20: stx_uid              uint32_t
//   offset  24: stx_gid              uint32_t
//   offset  28: stx_mode             uint16_t
//   offset  32: stx_ino              uint64_t
//   offset  40: stx_size             uint64_t
//   offset  48: stx_blocks           uint64_t
//   offset  56: stx_attributes_mask  uint64_t
//   offset  64: stx_atime            statx_timestamp (16 bytes)
//   offset  80: stx_btime            statx_timestamp
//   offset  96: stx_ctime            statx_timestamp
//   offset 112: stx_mtime            statx_timestamp
//   offset 128: stx_rdev_major/minor uint32_t x2
//   offset 136: stx_dev_major/minor  uint32_t x2
//   offset 144: stx_mnt_id           uint64_t
//   offset 152: stx_dio_mem_align    uint32_t
//   offset 156: stx_dio_offset_align uint32_t
//   offset 160: __spare3             uint64_t[12]
//   Total: 256 bytes

struct statx_timestamp {
  int64_t tv_sec;
  uint32_t tv_nsec;
  int32_t __reserved;
};

struct statx {
  uint32_t stx_mask;       // What results were written (STATX_* bits)
  uint32_t stx_blksize;    // Preferred general I/O size
  uint64_t stx_attributes; // Flags conveying extra information (unsupported)
  uint32_t stx_nlink;
  uint32_t stx_uid;
  uint32_t stx_gid;
  uint16_t stx_mode; // File type + permissions (low 16 bits of st_mode)
  uint16_t __spare0[1];
  uint64_t stx_ino;
  uint64_t stx_size;
  uint64_t stx_blocks; // Number of 512-byte blocks allocated
  uint64_t stx_attributes_mask;
  struct statx_timestamp stx_atime;
  struct statx_timestamp stx_btime; // Creation time (unsupported)
  struct statx_timestamp stx_ctime;
  struct statx_timestamp stx_mtime;
  uint32_t stx_rdev_major;
  uint32_t stx_rdev_minor;
  uint32_t stx_dev_major; // Device hosting the filesystem
  uint32_t stx_dev_minor;
  uint64_t stx_mnt_id; // Mount ID (unsupported)
  uint32_t stx_dio_mem_align;
  uint32_t stx_dio_offset_align;
  uint64_t __spare3[12]; // Spare space for future expansion
};

// stx_mask bits — Linux uapi values.
#define STATX_TYPE 0x00000001U        // stx_mode & S_IFMT
#define STATX_MODE 0x00000002U        // stx_mode & ~S_IFMT
#define STATX_NLINK 0x00000004U       // stx_nlink
#define STATX_UID 0x00000008U         // stx_uid
#define STATX_GID 0x00000010U         // stx_gid
#define STATX_ATIME 0x00000020U       // stx_atime
#define STATX_MTIME 0x00000040U       // stx_mtime
#define STATX_CTIME 0x00000080U       // stx_ctime
#define STATX_INO 0x00000100U         // stx_ino
#define STATX_SIZE 0x00000200U        // stx_size
#define STATX_BLOCKS 0x00000400U      // stx_blocks
#define STATX_BASIC_STATS 0x000007ffU // All of the above
#define STATX_BTIME 0x00000800U       // stx_btime (unsupported)
#define STATX_MNT_ID 0x00001000U      // stx_mnt_id (unsupported)
#define STATX_DIOALIGN 0x00002000U    // dio alignment (unsupported)

// statx flags — sync semantics (accepted, no-op: this FS has no writeback
// cache that could be out of sync). AT_STATX_SYNC_TYPE covering both bits is
// invalid (Linux). AT_SYMLINK_NOFOLLOW / AT_NO_AUTOMOUNT / AT_EMPTY_PATH live
// in kernel/bsd/kfcntl.h.
#define AT_STATX_SYNC_AS_STAT 0x0000
#define AT_STATX_FORCE_SYNC 0x2000
#define AT_STATX_DONT_SYNC 0x4000
#define AT_STATX_SYNC_TYPE 0x6000

#endif // KERNEL_BSD_STATX_ABI_H

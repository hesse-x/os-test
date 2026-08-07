/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_MOUNT_H
#define KERNEL_MOUNT_H

#include "kernel/bsd/inode.h"
#include "kernel/xcore/posix_types.h" // IWYU pragma: keep
#include "kernel/xcore/sparse.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// d_type constants (Linux DT_* values) — used by dir_emit and fstype
// getdents callbacks. No named constants existed before; fat32.c used
// inline magic numbers 4/8.
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12

// Pointer-encoded error helpers (Linux convention). The kernel did not
// define these before; vfs_resolve_user returns ERR_PTR(-errno) on
// failure so callers can distinguish "no mount matched" (NULL) from
// "user copy failed" (ERR_PTR).
#define IS_ERR_VALUE(x) ((unsigned long)(void *)(x) >= (unsigned long)(-4095UL))
#define ERR_PTR(e) ((void *)(long)(e))
#define PTR_ERR(p) ((long)(p))
#define IS_ERR(p) IS_ERR_VALUE((unsigned long)(void *)(p))

// Directory emit context — aligned with Linux dir_context/dir_emit model.
// fstype->getdents callbacks call dir_emit() for each entry; sys_getdents
// sets up ctx (buf/len/pos=f->offset) and writes back f->offset=ctx->pos.
struct dir_context {
  uint64_t pos;   // IN: f->offset; OUT: updated cursor
  void *buf;      // kernel buffer
  size_t len;     // buffer capacity
  size_t written; // bytes written so far
};

struct mount_entry; // forward: struct fstype.mount_root takes mount_entry*,
                    // defined below struct fstype

struct fstype {
  const char *name; // "fat32" / "devtmpfs" / "sysfs"
  struct inode *(*mount_root)(
      struct mount_entry *m); // return mountpoint root inode (inode_get'd)
  ssize_t (*getdents)(
      struct inode *dir,
      struct dir_context *ctx); // fops-layer per-inode; not in i_op
  // After refactor, lookup/mkdir/unlink/rmdir/stat global callbacks were
  // removed; they go through i_op instead.
};

// mount(2) flags — Linux x86-64 values (uapi linux/fs.h). Only the bits the
// kernel inspects are named here. MS_NOSUID is consumed by execve
// (setuid/setgid bit honored only on mounts without MS_NOSUID).
// MS_RDONLY/NODEV/NOEXEC are accepted and stored in mount_entry.m_flags but not
// yet enforced (no permission/execute-bit semantics in this FS); see todo.md.
// MS_REMOUNT/MS_BIND are not implemented and rejected with -ENOSYS so a caller
// cannot believe a remount/bind happened when it was silently dropped.
#define MS_RDONLY 0x00000001
#define MS_NOSUID 0x00000002
#define MS_NODEV 0x00000004
#define MS_NOEXEC 0x00000008
#define MS_BIND 0x00001000
#define MS_REMOUNT 0x80000000

#define MAX_MOUNTS 8
#define MNTPOINT_MAX 64
#define RELPATH_MAX 256

struct mount_entry {
  char mntpoint[MNTPOINT_MAX]; // "/" / "/dev" / "/sys"
  struct fstype *fs;
  void *fs_data;      // mount-private data (NULL for fat32/devtmpfs)
  struct inode *root; // per-mount root inode for filesystems such as tmpfs
  struct super_block sb;
  uint32_t m_flags; // MS_* bits accepted at mount(2) (MS_NOSUID consumed by
                    // execve; RDONLY/NODEV/NOEXEC stored, not yet enforced)
  bool in_use;
};

void mount_init(void);
void register_fstype(struct fstype *fs);
struct fstype *find_fstype_by_name(const char *name);
struct mount_entry *vfs_resolve(const char *path, char *relpath, size_t relcap);
struct mount_entry *vfs_resolve_user(const char __user *upath, char *relpath,
                                     size_t relcap);
struct mount_entry *mount_of_inode(struct inode *ip);
int mount_internal(struct fstype *fs, const char *target, void *fs_data,
                   uint32_t flags);
bool dir_emit(struct dir_context *ctx, const char *name, int namlen,
              uint64_t offset, uint64_t ino, unsigned int d_type);
int normalize_path(const char *in, char *out, size_t outcap);
int64_t sys_mount(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4,
                  int64_t arg5, int64_t unused);
int64_t sys_umount2(int64_t target, int64_t flags, int64_t, int64_t, int64_t,
                    int64_t);

#endif

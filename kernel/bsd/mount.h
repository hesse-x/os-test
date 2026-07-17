/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_MOUNT_H
#define KERNEL_MOUNT_H

#include "kernel/xcore/posix_types.h" // ssize_t
#include "kernel/xcore/sparse.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct inode;

/* d_type constants (Linux DT_* values) — used by dir_emit and fstype
 * getdents callbacks. No named constants existed before; fat32.c used
 * inline magic numbers 4/8. */
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12

/* Pointer-encoded error helpers (Linux convention). The kernel did not
 * define these before; vfs_resolve_user returns ERR_PTR(-errno) on
 * failure so callers can distinguish "no mount matched" (NULL) from
 * "user copy failed" (ERR_PTR). */
#define IS_ERR_VALUE(x) ((unsigned long)(void *)(x) >= (unsigned long)(-4095UL))
#define ERR_PTR(e) ((void *)(long)(e))
#define PTR_ERR(p) ((long)(p))
#define IS_ERR(p) IS_ERR_VALUE((unsigned long)(void *)(p))

/* Directory emit context — aligned with Linux dir_context/dir_emit model.
 * fstype->getdents callbacks call dir_emit() for each entry; sys_getdents
 * sets up ctx (buf/len/pos=f->offset) and writes back f->offset=ctx->pos. */
struct dir_context {
  uint64_t pos;   /* IN: f->offset; OUT: updated cursor */
  void *buf;      /* kernel buffer */
  size_t len;     /* buffer capacity */
  size_t written; /* bytes written so far */
};

struct kstat;
struct mount_entry; /* forward: struct fstype.mount_root takes mount_entry*,
                     * defined below struct fstype */

struct fstype {
  const char *name; /* "fat32" / "devtmpfs" / "sysfs" */
  struct inode *(*mount_root)(
      struct mount_entry *m); /* 返回挂载点根 inode(已 inode_get) */
  ssize_t (*getdents)(
      struct inode *dir,
      struct dir_context *ctx); /* fops 层 per-inode,不进 i_op */
  /* 重构后 lookup/mkdir/unlink/rmdir/stat 全局回调删除,改走 i_op。 */
};

#define MAX_MOUNTS 8
#define MNTPOINT_MAX 64
#define RELPATH_MAX 256

struct mount_entry {
  char mntpoint[MNTPOINT_MAX]; /* "/" / "/dev" / "/sys" */
  struct fstype *fs;
  void *fs_data; /* mount-private data (NULL for fat32/devtmpfs) */
  bool in_use;
};

void mount_init(void);
void register_fstype(struct fstype *fs);
struct fstype *find_fstype_by_name(const char *name);
struct mount_entry *vfs_resolve(const char *path, char *relpath, size_t relcap);
struct mount_entry *vfs_resolve_user(const char __user *upath, char *relpath,
                                     size_t relcap);
struct mount_entry *mount_of_inode(struct inode *ip);
int mount_internal(struct fstype *fs, const char *target, void *fs_data);
bool dir_emit(struct dir_context *ctx, const char *name, int namlen,
              uint64_t offset, uint64_t ino, unsigned int d_type);
int normalize_path(const char *in, char *out, size_t outcap);
int64_t sys_mount(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4,
                  int64_t arg5, int64_t unused);

#endif

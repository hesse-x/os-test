/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_VFS_H
#define KERNEL_VFS_H

#include <stddef.h> /* size_t (path_walk_parent lastcap) */
#include <stdint.h>

struct mount_entry; /* forward: vfs.h is included before mount.h/inode.h in
                     * some translation units (e.g. proc.c via devtmpfs.h);
                     * the new path_walk prototypes take mount_entry* / inode**
                     */
struct inode;

void vfs_init(void);

struct inode *path_walk(struct mount_entry *m, const char *relpath);
int path_walk_parent(struct mount_entry *m, const char *relpath,
                     struct inode **out_parent, char *lastname, size_t lastcap);

/* S07: dirfd-relative resolution. path_walk_from / path_walk_parent_from walk
 * `relpath` starting from an explicit start inode (the dirfd's directory inode)
 * instead of a mount root. Same +1 refcount contract as path_walk/_parent.
 * relpath must NOT be absolute (caller strips a leading '/' / falls back to
 * root for absolute paths). No fs-internal `..` crossing a mount boundary —
 * callers pass a path already within the start inode's subtree. */
struct inode *path_walk_from(struct inode *start, const char *relpath);
int path_walk_parent_from(struct inode *start, const char *relpath,
                          struct inode **out_parent, char *lastname,
                          size_t lastcap);
/* S07: dirfd → start directory inode (+1, caller puts) or ERR_PTR(-errno).
 * AT_FDCWD → root mount root (no per-process CWD exists). Used by the *at
 * syscalls in syscall.c (mkdirat/unlinkat/renameat). */
struct inode *resolve_dirfd_start(int dirfd);
struct inode *vfs_open_kern(const char *kpath);

int64_t sys_open(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                 int64_t unused2, int64_t unused3);
int64_t sys_stat(int64_t arg1, int64_t arg2, int64_t unused1, int64_t unused2,
                 int64_t unused3, int64_t unused4);
int64_t sys_mkdir(int64_t arg1, int64_t arg2, int64_t unused1, int64_t unused2,
                  int64_t unused3, int64_t unused4);
int64_t sys_mknod(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                  int64_t unused2, int64_t unused3);
int64_t sys_unlink(int64_t arg1, int64_t unused1, int64_t unused2,
                   int64_t unused3, int64_t unused4, int64_t unused5);
int64_t sys_rmdir(int64_t arg1, int64_t unused1, int64_t unused2,
                  int64_t unused3, int64_t unused4, int64_t unused5);
int64_t sys_rename(int64_t arg1, int64_t arg2, int64_t unused1, int64_t unused2,
                   int64_t unused3, int64_t unused4);
int64_t sys_dev_create(int64_t arg1, int64_t arg2, int64_t unused1,
                       int64_t unused2, int64_t unused3, int64_t unused4);
int64_t sys_getdents(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                     int64_t unused2, int64_t unused3);

#endif

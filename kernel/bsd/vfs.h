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
struct vfs_timespec64;

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

/* follow_symlink:跟随 LNK inode 的 target 串,返回解析后目标 inode(+1,调用者
 * put)或 ERR_PTR(-errno)。depth 防 target 循环。chmod/chown 等 syscall 复用
 * 末段 symlink 跟随(默认跟随;AT_SYMLINK_NOFOLLOW 时调用方不调本函数)。 */
struct inode *follow_symlink(struct inode *lnk, int *depth);

/* inode_permission:按 check_uid/check_gid 判定
 * mask(R_OK/W_OK/X_OK/F_OK)权限(Q4)。 root 放行经
 * capable(CAP_DAC_OVERRIDE)——仍按 EFFECTIVE uid(current_proc->euid)判, 不随
 * check_uid 走(setuid-root 程序 ruid=nobody 仍应放行);check_uid/check_gid
 * 仅驱动 owner/group/other 位选择。access(2) 传 real
 * uid,faccessat(AT_EACCESS)/eaccess 传 effective uid,其余(open/utimensat 等)传
 * euid。返 0=允许,负=-EACCES/-ENOENT。 通用实现:各 fs .permission 暂置 NULL,VFS
 * 回退到本函数。 */
int inode_permission(struct inode *ip, int mask, uint32_t check_uid,
                     uint32_t check_gid);
/* generic_update_time:VFS 层默认时间戳更新(内存态,Q5)。按 which(ATIME_BIT/
 * MTIME_BIT/CTIME_BIT 组合)写非 OMIT 的时间戳。各 fs .update_time 可置 NULL,
 * VFS 回退到此。 */
int generic_update_time(struct inode *ip, struct vfs_timespec64 at,
                        struct vfs_timespec64 mt, struct vfs_timespec64 ct,
                        int which);

/* S19 §7: kernel-mode inode-read helper for execve/elf_loader. Reads `count`
 * bytes at `offset` from inode `ip` into a KERNEL-space buffer `buf` and
 * returns the byte count (>=0) or a negative errno. Unlike f_op->read (fd-I/O,
 * advances f->offset, takes a struct file), this is an inode+offset read with
 * no fd and no offset advance — suited to loading an ELF image into a kmalloc'd
 * buffer. Dispatches by inode type/fs: fat32 regular files via fat32_read;
 * directories are -EISDIR; everything else (devtmpfs char devices, sysfs, tmpfs
 * — tmpfs kernel-read is deferred, see doc/design/todo.md) is -ENOEXEC since
 * none is executable. */
int vfs_read_kernel(struct inode *ip, uint64_t offset, void *buf, size_t count);

int64_t sys_open(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                 int64_t unused2, int64_t unused3);
int64_t sys_stat(int64_t arg1, int64_t arg2, int64_t unused1, int64_t unused2,
                 int64_t unused3, int64_t unused4);
int64_t sys_lstat(int64_t arg1, int64_t arg2, int64_t unused1, int64_t unused2,
                  int64_t unused3, int64_t unused4);
struct statx;
/* statx 核心：SYS_STATX 与 legacy stat 薄封装的共同实现。kpath 为内核字
 * 符串（调用方已完成 copy_from_user）。 */
int vfs_statx(int dirfd, const char *kpath, unsigned flags, struct statx *stx);
int64_t sys_statx(int64_t dirfd, int64_t path, int64_t flags, int64_t mask,
                  int64_t buf, int64_t unused);
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

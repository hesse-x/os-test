/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_VFS_H
#define KERNEL_VFS_H

#include <stddef.h> // size_t (path_walk_parent lastcap)
#include <stdint.h>

struct mount_entry; // forward: vfs.h is included before mount.h/inode.h in
                    // some translation units (e.g. proc.c via devtmpfs.h);
                    // the new path_walk prototypes take mount_entry* / inode**
struct inode;
struct vfs_timespec64;

void vfs_init(void);

struct inode *path_walk(struct mount_entry *m, const char *relpath);
int path_walk_parent(struct mount_entry *m, const char *relpath,
                     struct inode **out_parent, char *lastname, size_t lastcap);

// S07: dirfd-relative resolution. path_walk_from / path_walk_parent_from walk
// `relpath` starting from an explicit start inode (the dirfd's directory inode)
// instead of a mount root. Same +1 refcount contract as path_walk/_parent.
// relpath must NOT be absolute (caller strips a leading '/' / falls back to
// root for absolute paths). No fs-internal `..` crossing a mount boundary —
// callers pass a path already within the start inode's subtree.
struct inode *path_walk_from(struct inode *start, const char *relpath);
int path_walk_parent_from(struct inode *start, const char *relpath,
                          struct inode **out_parent, char *lastname,
                          size_t lastcap);
// S07: dirfd → start directory inode (+1, caller puts) or ERR_PTR(-errno).
// AT_FDCWD → root mount root (no per-process CWD exists). Used by the *at
// syscalls in syscall.c (mkdirat/unlinkat/renameat).
struct inode *resolve_dirfd_start(int dirfd);
struct inode *vfs_open_kern(const char *kpath);

// follow_symlink: follow the LNK inode's target string, returning the resolved
// target inode (+1, caller puts) or ERR_PTR(-errno). depth guards against
// target loops. chmod/chown etc. reuse final-segment symlink following (follow
// by default; with AT_SYMLINK_NOFOLLOW callers don't invoke this).
struct inode *follow_symlink(struct inode *lnk, int *depth);

// inode_permission: judge the mask (R_OK/W_OK/X_OK/F_OK) permission by
// check_uid/check_gid (Q4). Root privilege passes via capable(CAP_DAC_OVERRIDE)
// — still judged by the EFFECTIVE uid (current_proc->euid), not by check_uid
// (a setuid-root program with ruid=nobody still passes). check_uid/check_gid
// only drive owner/group/other bit selection. access(2) passes the real uid;
// faccessat(AT_EACCESS)/eaccess pass the effective uid; the rest
// (open/utimensat etc.) pass the euid. Returns 0=allowed, negative=
// -EACCES/-ENOENT. Generic implementation: each fs's .permission is set NULL
// for now and VFS falls back to this function.
int inode_permission(struct inode *ip, int mask, uint32_t check_uid,
                     uint32_t check_gid);
// generic_update_time: VFS-layer default timestamp update (in-memory, Q5). Per
// the which bits (ATIME_BIT/MTIME_BIT/CTIME_BIT combination) writes the
// non-OMIT timestamps. Each fs's .update_time may be NULL and VFS falls back
// here.
int generic_update_time(struct inode *ip, struct vfs_timespec64 at,
                        struct vfs_timespec64 mt, struct vfs_timespec64 ct,
                        int which);

// S19 §7: kernel-mode inode-read helper for execve/elf_loader. Reads `count`
// bytes at `offset` from inode `ip` into a KERNEL-space buffer `buf` and
// returns the byte count (>=0) or a negative errno. Unlike f_op->read (fd-I/O,
// advances f->offset, takes a struct file), this is an inode+offset read with
// no fd and no offset advance — suited to loading an ELF image into a kmalloc'd
// buffer. Dispatches by inode type/fs: fat32 regular files via fat32_read;
// directories are -EISDIR; everything else (devtmpfs char devices, sysfs, tmpfs
// — tmpfs kernel-read is deferred, see doc/design/todo.md) is -ENOEXEC since
// none is executable.
int vfs_read_kernel(struct inode *ip, uint64_t offset, void *buf, size_t count);

int64_t sys_open(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                 int64_t unused2, int64_t unused3);
int64_t sys_stat(int64_t arg1, int64_t arg2, int64_t unused1, int64_t unused2,
                 int64_t unused3, int64_t unused4);
int64_t sys_lstat(int64_t arg1, int64_t arg2, int64_t unused1, int64_t unused2,
                  int64_t unused3, int64_t unused4);
struct statx;
// statx core: the shared implementation of SYS_STATX and the legacy stat thin
// wrappers. kpath is a kernel string (the caller has already done
// copy_from_user).
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

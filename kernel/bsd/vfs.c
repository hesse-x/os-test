/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/bsd/vfs.h"
#include "arch/x64/smp.h"
#include "arch/x64/utils.h"
#include "kernel/bsd/devtmpfs.h"
#include "kernel/bsd/fat32.h"
#include "kernel/bsd/fops.h"
#include "kernel/bsd/inode.h"
#include "kernel/bsd/kfcntl.h"
#include "kernel/bsd/mount.h"
#include "kernel/bsd/page_cache.h"
#include "kernel/bsd/proc.h"
#include "kernel/bsd/procfs.h"
#include "kernel/bsd/pty.h"
#include "kernel/bsd/syscall.h"
#include "kernel/bsd/sysfs.h"
#include "kernel/bsd/tmpfs.h"
#include "kernel/bsd/types.h"
#include "kernel/driver/blk_dev.h"
#include "kernel/driver/dma_buf.h"
#include "kernel/driver/drm/drm_core.h"
#include "kernel/driver/serial.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/kasan.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/mutex.h"
#include "kernel/xcore/rcu.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/xtask.h"
#include "xos/page.h"
#include <kernel/bsd/stat_abi.h>
#include <kernel/bsd/statx_abi.h>
#include <stdbool.h>
#include <stddef.h>
#include <xos/capability.h>
#include <xos/errno.h>

// DRM major (used only for stat device numbers; same value as virtio_gpu.c
// DRM_MAJOR; 226 is DRM semantics, not a devtmpfs common-layer concern, so each
// .c defines its own at the top rather than putting it in a shared header).
#define DRM_MAJOR_FOR_STAT 226

void vfs_init(void) {
  // inode_init, page_cache_init, devtmpfs_init are called in kernel_main before
  // driver_init. drm_dev_register() is called from virtio_gpu_init
  // (driver_init).
  mount_init();
  serial_dev_register();
  pty_init();

  int rc = block_init_ahci();
  struct block_partition *root_part =
      block_partition_get(block_primary_device(), 2);
  if (rc == 0 && root_part)
    rc = fat32_init(root_part);
  if (rc == 0) {
    printk(LOG_INFO, "vfs_init: FAT32 inited on sda2\n");
    register_fstype(&fat32_fstype);
    register_fstype(&devtmpfs_fstype);
    register_fstype(&sysfs_fstype);
    register_fstype(&tmpfs_fstype);
    register_fstype(&procfs_fstype);
    sysfs_init();
    mount_internal(&fat32_fstype, "/", NULL, 0);
    // Create /dev directory entry on FAT32 root so getdents("/") sees it.
    // fat32_mkdir is not idempotent (it allocates a cluster unconditionally),
    // so only create when the entry is missing.
    {
      uint8_t ksb[256];
      if (fat32_stat("/dev", ksb) != 0)
        fat32_mkdir("/dev");
    }
    mount_internal(&devtmpfs_fstype, "/dev", NULL, 0);
    // Create /sys directory on FAT32 root for getdents("/") visibility
    {
      uint8_t ksb[256];
      if (fat32_stat("/sys", ksb) != 0)
        fat32_mkdir("/sys");
    }
    mount_internal(&sysfs_fstype, "/sys", sysfs_root_node(), 0);
    // procfs (procfs.md §2.4): create /proc directory, mount procfs_fstype.
    {
      uint8_t ksb[256];
      if (fat32_stat("/proc", ksb) != 0)
        fat32_mkdir("/proc");
    }
    procfs_init();
    mount_internal(&procfs_fstype, "/proc", procfs_root_node(), 0);
    // Create /run directory on FAT32 root for getdents("/") visibility,
    // then mount tmpfs on /run (in-memory fs, prerequisite for udevd
    // db/socket).
    {
      uint8_t ksb[256];
      if (fat32_stat("/run", ksb) != 0)
        fat32_mkdir("/run");
    }
    mount_internal(&tmpfs_fstype, "/run", NULL, 0);
    // POSIX shm_open maps names to /dev/shm. Keep it on a distinct tmpfs
    // mount so shared-memory objects cannot collide with /run contents.
    devtmpfs_mkdir("shm");
    mount_internal(&tmpfs_fstype, "/dev/shm", NULL, 0);
    block_publish_devtmpfs();
  }
  if (rc != 0) {
    printk(LOG_ERROR, "vfs_init: FAT32 init failed on all ports\n");
    return;
  }
}

// path_walk: walk segment-by-segment lookup to the target inode (already
// inode_get, +1, caller puts). relpath stays within a single mount (vfs_resolve
// has stripped the mount-point prefix); no in-fs `..` crossing mounts. Middle
// segments must be INODE_DIR, else NULL is returned. The final segment's type
// is not checked.
//
// §3.3.3 symlink following: a middle segment that is INODE_LNK is resolved via
// follow_symlink's target string to replace dir and continue (matching Linux's
// follow of walk middle components); a final-segment LNK is returned as-is —
// callers like stat/access decide whether to follow the final segment
// (AT_SYMLINK_NOFOLLOW / readlink takes the link inode itself). SYMLINK_MAX
// guards against target loops → ELOOP.
#define SYMLINK_MAX 40 // matches Linux MAXSYMLINKS

// follow_symlink: follow the LNK inode's target string (via i_op->readlink),
// returning the resolved target inode (+1, caller puts). depth guards against
// target loops → ELOOP. An absolute target re-resolves from the root mount
// (vfs_resolve); a relative target resolves from the lnk's parent dir (this OS
// tmpfs inodes have no parent_dir back-reference, so relative targets are rare
// — fall back to resolving from root). Not static: chmod/chown etc. reuse
// final-segment symlink following (same pattern as vfs_statx).
struct inode *follow_symlink(struct inode *lnk, int *depth) {
  if (!lnk || lnk->type != INODE_LNK || !lnk->i_op || !lnk->i_op->readlink)
    return ERR_PTR(-EINVAL);
  if (++(*depth) > SYMLINK_MAX)
    return ERR_PTR(-ELOOP);
  char target[256];
  int n = lnk->i_op->readlink(lnk, target, sizeof(target));
  if (n < 0)
    return ERR_PTR(n);
  if (n >= (int)sizeof(target))
    return ERR_PTR(-ENAMETOOLONG);
  target[n] = '\0';
  if (target[0] == '/') {
    char relpath[256];
    struct mount_entry *m = vfs_resolve(target, relpath, sizeof(relpath));
    if (!m)
      return ERR_PTR(-ENOENT);
    return path_walk(m, relpath); // +1
  }
  // Relative target: this OS tmpfs inode has no parent_dir back-reference, so
  // resolve from root (relative symlink targets are rare; absolute targets are
  // the common case).
  char relpath[256];
  struct mount_entry *m = vfs_resolve(target, relpath, sizeof(relpath));
  if (!m)
    return ERR_PTR(-ENOENT);
  return path_walk(m, relpath); // +1
}

struct inode *path_walk(struct mount_entry *m, const char *relpath) {
  if (!m->fs->mount_root)
    return NULL;
  struct inode *dir = m->fs->mount_root(m); // root inode (+1)
  if (!dir)
    return NULL;
  const char *p = relpath;
  int sym_depth = 0; // §3.3.3 SYMLINK_MAX guards against target loops
  while (*p) {
    while (*p == '/')
      p++;
    if (!*p)
      break; // trailing slash, dir is the target
    const char *seg = p;
    while (*p && *p != '/')
      p++;
    // Whether there are more non-slash segments (determines if this segment is
    // a "middle" segment): after skipping trailing slashes, *p is non-empty.
    const char *after = p;
    while (*after == '/')
      after++;
    int has_more = (*after != '\0');
    int seglen = p - seg;
    char name[256];
    if (seglen >= 256) {
      inode_put(dir);
      return NULL;
    }
    __memcpy(name, seg, seglen);
    name[seglen] = '\0';
    if (!dir->i_op || !dir->i_op->lookup) {
      inode_put(dir);
      return NULL;
    }
    struct inode *next = dir->i_op->lookup(dir, name); // +1
    inode_put(dir); // release the previous segment (matching dget/dput)
    dir = next;
    if (!dir)
      return NULL;
    // §3.3.3 middle-segment symlink following: this segment is not the last and
    // is a LNK → follow_symlink replaces dir with the target resolution (+1).
    // A final-segment LNK is returned as-is (stat decides whether to follow).
    if (has_more && dir->type == INODE_LNK) {
      struct inode *resolved = follow_symlink(dir, &sym_depth);
      inode_put(dir);
      dir = resolved;
      if (IS_ERR(dir))
        return NULL; // ELOOP/ENOENT/ENAMETOOLONG → resolution failure
    }
  }
  return dir; // target, +1, caller puts
}

// path_walk_parent: walk to the penultimate segment, return the parent
// directory inode (+1, caller puts) and write the last segment name into
// lastname. An empty relpath or "/" is explicitly rejected.
int path_walk_parent(struct mount_entry *m, const char *relpath,
                     struct inode **out_parent, char *lastname,
                     size_t lastcap) {
  *out_parent = NULL;
  lastname[0] = '\0';
  if (!relpath[0] || (relpath[0] == '/' && relpath[1] == '\0'))
    return -EBUSY; // root has no parent, no lastname (mkdir/rmdir "/" → -EBUSY)
  if (!m->fs->mount_root)
    return -ENOENT;
  struct inode *dir = m->fs->mount_root(m);
  if (!dir)
    return -ENOENT;
  const char *p = relpath;
  while (*p == '/')
    p++;
  const char *seg = p;
  while (*p && *p != '/')
    p++;
  int seglen = p - seg;
  for (;;) {
    const char *next = p;
    while (*next == '/')
      next++;
    if (!*next) {
      // seg is the last segment
      if (seglen >= (int)lastcap) {
        inode_put(dir);
        return -ENAMETOOLONG;
      }
      __memcpy(lastname, seg, seglen);
      lastname[seglen] = '\0';
      if (dir->type != INODE_DIR) {
        inode_put(dir);
        return -ENOTDIR;
      }
      *out_parent = dir;
      return 0;
    }
    char name[256];
    if (seglen >= 256) {
      inode_put(dir);
      return -ENAMETOOLONG;
    }
    __memcpy(name, seg, seglen);
    name[seglen] = '\0';
    if (dir->type != INODE_DIR) {
      inode_put(dir);
      return -ENOTDIR;
    }
    if (!dir->i_op || !dir->i_op->lookup) {
      inode_put(dir);
      return -ENOTDIR;
    }
    struct inode *child = dir->i_op->lookup(dir, name); // +1
    inode_put(dir);
    dir = child;
    if (!dir)
      return -ENOENT;
    p = next;
    seg = p;
    while (*p && *p != '/')
      p++;
    seglen = p - seg;
  }
}

// path_walk_from: walk segment-by-segment, resolving relpath from a given start
// inode (+1, caller puts). Same semantics as path_walk, but the start is the
// directory inode pointed at by dirfd rather than the mount root. relpath must
// not start with '/' (callers fall back to root resolution for absolute paths).
// Middle segments must be INODE_DIR.
struct inode *path_walk_from(struct inode *start, const char *relpath) {
  if (!start)
    return NULL;
  struct inode *dir = inode_get(start);
  const char *p = relpath;
  int sym_depth = 0; // §3.3.3 SYMLINK_MAX
  while (*p) {
    while (*p == '/')
      p++;
    if (!*p)
      break; // trailing slash, dir is the target
    const char *seg = p;
    while (*p && *p != '/')
      p++;
    const char *after = p;
    while (*after == '/')
      after++;
    int has_more = (*after != '\0');
    int seglen = p - seg;
    char name[256];
    if (seglen >= 256) {
      inode_put(dir);
      return NULL;
    }
    __memcpy(name, seg, seglen);
    name[seglen] = '\0';
    if (dir->type != INODE_DIR || !dir->i_op || !dir->i_op->lookup) {
      inode_put(dir);
      return NULL;
    }
    struct inode *next = dir->i_op->lookup(dir, name); // +1
    inode_put(dir);
    dir = next;
    if (!dir)
      return NULL;
    // §3.3.3 middle-segment symlink following (same as path_walk). Relative
    // targets resolve from root (this OS tmpfs inode has no parent_dir
    // back-reference).
    if (has_more && dir->type == INODE_LNK) {
      struct inode *resolved = follow_symlink(dir, &sym_depth);
      inode_put(dir);
      dir = resolved;
      if (IS_ERR(dir))
        return NULL;
    }
  }
  return dir; // +1, caller puts
}

// path_walk_parent_from: same as path_walk_parent, but the start is a start
// inode (+1 parent, caller puts) + last segment name.
int path_walk_parent_from(struct inode *start, const char *relpath,
                          struct inode **out_parent, char *lastname,
                          size_t lastcap) {
  *out_parent = NULL;
  lastname[0] = '\0';
  if (!start)
    return -ENOENT;
  if (!relpath[0] || (relpath[0] == '/' && relpath[1] == '\0'))
    return -EBUSY; // root has no parent, no lastname
  struct inode *dir = inode_get(start);
  const char *p = relpath;
  while (*p == '/')
    p++;
  const char *seg = p;
  while (*p && *p != '/')
    p++;
  int seglen = p - seg;
  for (;;) {
    const char *next = p;
    while (*next == '/')
      next++;
    if (!*next) {
      // seg is the last segment
      if (seglen >= (int)lastcap) {
        inode_put(dir);
        return -ENAMETOOLONG;
      }
      __memcpy(lastname, seg, seglen);
      lastname[seglen] = '\0';
      if (dir->type != INODE_DIR) {
        inode_put(dir);
        return -ENOTDIR;
      }
      *out_parent = dir;
      return 0;
    }
    char name[256];
    if (seglen >= 256) {
      inode_put(dir);
      return -ENAMETOOLONG;
    }
    __memcpy(name, seg, seglen);
    name[seglen] = '\0';
    if (dir->type != INODE_DIR) {
      inode_put(dir);
      return -ENOTDIR;
    }
    if (!dir->i_op || !dir->i_op->lookup) {
      inode_put(dir);
      return -ENOTDIR;
    }
    struct inode *child = dir->i_op->lookup(dir, name); // +1
    inode_put(dir);
    dir = child;
    if (!dir)
      return -ENOENT;
    p = next;
    seg = p;
    while (*p && *p != '/')
      p++;
    seglen = p - seg;
  }
}

// vfs_open_kern: kernel-mode path resolution, returns +1 inode or NULL (no fd
// installed, no user copy).
struct inode *vfs_open_kern(const char *kpath) {
  char relpath[256];
  struct mount_entry *m = vfs_resolve(kpath, relpath, sizeof(relpath));
  if (!m)
    return NULL;
  return path_walk(m, relpath); // +1, caller puts
}

// inode_permission: judge mask permission by check_uid/check_gid (Q4). This OS
// has the full permission ladder (proc.h uid/euid/suid/gid/egid/sgid, default
// 0=root; test_setuid_saved proves the ladder really runs), so the bit-mask
// decision is not a naive "root always passes". Root privilege passes via
// capable(CAP_DAC_OVERRIDE) — still judged by the EFFECTIVE uid
// (current_proc->euid), not by check_uid: a setuid-root program with
// ruid=nobody must still have root privilege apply (effective-credential
// semantics). check_uid/check_gid only drive owner/group/other bit selection:
// access(2) passes the real uid, faccessat(AT_EACCESS)/eaccess pass the
// effective uid, the rest (open/utimensat) pass the euid. Returns 0=allowed,
// negative=-EACCES/-ENOENT.
int inode_permission(struct inode *ip, int mask, uint32_t check_uid,
                     uint32_t check_gid) {
  if (!ip)
    return -ENOENT;
  if (mask == F_OK)
    return 0; // existence: path_walk success implies it exists
  if (capable(CAP_DAC_OVERRIDE))
    return 0; // root pass (CAP_DAC_OVERRIDE; by euid, not check_uid)
  // Non-root: use mode's owner/group/other bits. check_uid matching owner →
  // owner bits; else check_gid matching gid → group bits; else other bits.
  uint32_t mode = ip->mode;
  uint32_t bits = (check_uid == ip->uid)   ? (mode >> 6) & 7
                  : (check_gid == ip->gid) ? (mode >> 3) & 7
                                           : mode & 7;
  if ((mask & R_OK) && !(bits & R_OK))
    return -EACCES;
  if ((mask & W_OK) && !(bits & W_OK))
    return -EACCES;
  if ((mask & X_OK) && !(bits & X_OK))
    return -EACCES;
  return 0;
}

// generic_update_time: VFS-layer default timestamp update (in-memory, Q5).
// Writes the non-OMIT timestamps per the `which` bits. The OMIT sentinel is
// interpreted by the caller (sys_utimensat); here only explicitly-passed values
// are written. A filesystem .update_time may be NULL and VFS falls back here.
int generic_update_time(struct inode *ip, struct vfs_timespec64 at,
                        struct vfs_timespec64 mt, struct vfs_timespec64 ct,
                        int which) {
  if (!ip)
    return -ENOENT;
  if (((which & ATIME_BIT) && at.tv_nsec >= 1000000000U) ||
      ((which & MTIME_BIT) && mt.tv_nsec >= 1000000000U) ||
      ((which & CTIME_BIT) && ct.tv_nsec >= 1000000000U))
    return -EINVAL;
  mutex_lock(&ip->i_lock);
  if ((which & ATIME_BIT))
    ip->atime = at;
  if ((which & MTIME_BIT))
    ip->mtime = mt;
  if ((which & CTIME_BIT))
    ip->ctime = ct;
  mutex_unlock(&ip->i_lock);
  return 0;
}

// S19 §7: kernel-mode inode read for execve. Only regular files backed by a
// real filesystem (fat32) are readable here; char devices / pseudo-fs / tmpfs
// are not executable, so execve bails with -ENOEXEC before touching the inode
// data. tmpfs kernel-read (for memfd-style tmpfs binaries) is deferred — the
// interface stays generic so adding it later does not touch execve again.
int vfs_read_kernel(struct inode *ip, uint64_t offset, void *buf,
                    size_t count) {
  if (!ip || !buf)
    return -EINVAL;
  if (ip->type == INODE_DIR)
    return -EISDIR;
  if (ip->type != INODE_REGULAR)
    return -ENOEXEC;
  if (!ip->i_fop || !ip->i_fop->read_at)
    return -ENOEXEC;
  struct file file = {.type = FD_REGULAR,
                      .flags = O_RDONLY,
                      .inode = ip,
                      .offset = offset,
                      .f_op = ip->i_fop};
  return (int)ip->i_fop->read_at(NULL, &file, buf, count, offset);
}

// sys_open(path, flags, mode) — SYS_OPEN
int64_t sys_open(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                 int64_t unused2, int64_t unused3) {
  const char __user *upath = (const char __user *__force)arg1;
  int flags = (int)arg2;

  // 1. Resolve via mount table (longest-prefix match)
  char relpath[256];
  struct mount_entry *m = vfs_resolve_user(upath, relpath, sizeof(relpath));
  if (IS_ERR(m))
    return PTR_ERR(m);
  if (!m)
    return (int64_t)-ENOENT;

  bool wants_write = (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC)) != 0;
  if ((m->m_flags & MS_RDONLY) && wants_write)
    return (int64_t)-EROFS;

  // 2. devtmpfs device files: delegate to devtmpfs_open so the fd is
  // created as FD_DEV and ops->open (ptmx/pts, serial, etc.) runs.
  // The bare "/dev" directory (relpath empty) falls through to the
  // generic directory path below.
  if (m->fs == &devtmpfs_fstype && relpath[0] != '\0') {
    if (m->m_flags & MS_NODEV)
      return (int64_t)-EACCES;
    int64_t dev_ret = devtmpfs_open(current_task, relpath, flags, m);
    return dev_ret;
  }

  // 3. Look up an existing entry (segment-by-segment path_walk)
  struct inode *ip = path_walk(m, relpath); // +1
  if (ip) {
    // O_EXCL: file must not already exist.
    if ((flags & O_CREAT) && (flags & O_EXCL)) {
      inode_put(ip);
      return (int64_t)-EEXIST;
    }
    // O_TRUNC: go through i_op->setattr (not hardcoded to fat32). Only for
    // INODE_REGULAR.
    if ((flags & O_TRUNC) && ip->type == INODE_REGULAR && ip->size > 0) {
      if (!ip->i_op || !ip->i_op->setattr) {
        inode_put(ip);
        return (
            int64_t)-EPERM; // matches Linux notify_change: no setattr → EPERM
      }
      ip->i_op->setattr(ip, 0); // lock held internally by setattr (§6.6)
    }
  } else if (flags & O_CREAT) {
    // Not existing + O_CREAT: path_walk_parent gets the parent + last segment
    // name
    char lastname[256];
    struct inode *parent = NULL;
    int rc = path_walk_parent(m, relpath, &parent, lastname, sizeof(lastname));
    if (rc) {
      if (parent)
        inode_put(parent);
      return (int64_t)rc;
    }
    if (!parent->i_op || !parent->i_op->create) {
      inode_put(parent);
      return (int64_t)-EACCES;
    }
    // S08: apply umask (mode & ~umask), set owner = current process uid/gid.
    // umask is applied here rather than in create: create doesn't know the
    // caller's umask.
    int eff_mode = (int)arg3 & 0777;
    eff_mode = eff_mode & ~(int)current_proc->umask;
    ip = parent->i_op->create(parent, lastname, eff_mode); // +1 new inode
    inode_put(parent); // return path_walk_parent's parent
    if (IS_ERR(ip))
      return PTR_ERR(ip);
    if (!ip)
      return (int64_t)-ENOMEM;
    // S08: new file owner = creating process (not hardcoded 0/caller). Only
    // regular-file create; sockets go through vfs_mknod_socket (mknod) path.
    ip->mode = (ip->mode & ~0777) | (uint32_t)eff_mode;
    ip->uid = current_proc->uid;
    ip->gid = current_proc->gid;
  } else {
    return (int64_t)-ENOENT;
  }
  // ip is now +1 (from path_walk or create).
  ip->mount = m; // mount set lazily (§6 invariant 2: only sys_open sets it)

  // Reject write access to directories (POSIX EISDIR).
  if (ip->type == INODE_DIR &&
      (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC))) {
    inode_put(ip);
    return (int64_t)-EISDIR;
  }

  // O_DIRECTORY: caller requires a directory; non-dir → ENOTDIR (Linux).
  if ((flags & O_DIRECTORY) && ip->type != INODE_DIR) {
    inode_put(ip);
    return (int64_t)-ENOTDIR;
  }

  // Linux semantics: open()ing a socket file returns ENXIO (any flags).
  // Socket files can only be accessed via bind/connect, not opened for fd
  // read/write.
  if (ip->type == INODE_SOCKET) {
    inode_put(ip);
    return (int64_t)-ENXIO;
  }

  // 4. Allocate fd (under fd_lock)
  xtask *proc = current_task;
  files *files = proc->proc->files;
  spinlock *fdlk = &files->fd_lock;
  spin_lock(fdlk);
  int fd = alloc_fd(files, 0);
  if (fd < 0) {
    spin_unlock(fdlk);
    inode_put(ip);
    return (int64_t)-EMFILE;
  }

  // 5. Allocate struct file
  struct file *f = (struct file *)kmalloc(sizeof(struct file));
  if (!f) {
    spin_unlock(fdlk);
    inode_put(ip);
    return (int64_t)-ENOMEM;
  }
  __memset(f, 0, sizeof(*f));
  refcount_set(&f->f_count, 1);

  // 6. Set up fd entry
  f->f_op = ip->i_fop;
  f->mount = m;
  atomic_inc(&m->sb.active_files);

  if (ip->type == INODE_DIR) {
    f->type = FD_DIR;
    f->flags = O_RDONLY;
    f->inode = ip;
    f->offset = 0; // directory scan position
  } else {
    f->type = FD_REGULAR;
    f->flags = flags & (O_RDONLY | O_WRONLY | O_RDWR | O_APPEND | O_NONBLOCK);
    f->inode = ip;
    f->offset = 0;
  }
  fd_install(files, fd, f);
  // S06: O_CLOEXEC is an fd-level attribute — set the per-fd bitmap bit, not
  // the shared file's flags (a later dup would otherwise inherit it wrongly).
  fd_set_cloexec(files, fd, (flags & O_CLOEXEC) ? 1 : 0);
  spin_unlock(fdlk);
  return (int64_t)fd;
}

// ===================== statx core =====================
// vfs_statx(dirfd, kpath, flags, stx) — the single metadata-fetch core,
// exposed directly by SYS_STATX; the legacy SYS_STAT/SYS_FSTAT/SYS_NEWFSTATAT
// are thin wrappers narrowing to struct kstat. Resolution matches Linux statx:
//   AT_EMPTY_PATH + "" → stat the fd itself (per-fd-type fill)
//   absolute path     → longest-prefix match on the mount table
//   relative path     → resolve from dirfd (AT_FDCWD ≡ root; the kernel has no
//                        per-process CWD, libc concatenates an absolute path
//                        before calling)
// AT_SYMLINK_NOFOLLOW/AT_NO_AUTOMOUNT accepted but no-op (this OS has
// symlinks). The fs layer getattr still fills struct kstat (unchanged); here
// statx_from_kstat expands it; stx_mask only reports STATX_BASIC_STATS —
// btime/attributes/mnt_id are not provided, callers must not read them.

// kstat → statx expansion. stx_mode/stx_nlink narrowing is safe (value range
// far smaller than the field width).
static void statx_from_kstat(struct statx *stx, const struct kstat *ks) {
  __memset(stx, 0, sizeof(*stx));
  stx->stx_mask = STATX_BASIC_STATS;
  stx->stx_blksize = (uint32_t)ks->st_blksize;
  stx->stx_nlink = (uint32_t)ks->st_nlink;
  stx->stx_uid = ks->st_uid;
  stx->stx_gid = ks->st_gid;
  stx->stx_mode = (uint16_t)ks->st_mode;
  stx->stx_ino = ks->st_ino;
  stx->stx_size = (uint64_t)ks->st_size;
  stx->stx_blocks = (uint64_t)ks->st_blocks;
  stx->stx_atime.tv_sec = ks->st_atim.tv_sec;
  stx->stx_atime.tv_nsec = (uint32_t)ks->st_atim.tv_nsec;
  stx->stx_mtime.tv_sec = ks->st_mtim.tv_sec;
  stx->stx_mtime.tv_nsec = (uint32_t)ks->st_mtim.tv_nsec;
  stx->stx_ctime.tv_sec = ks->st_ctim.tv_sec;
  stx->stx_ctime.tv_nsec = (uint32_t)ks->st_ctim.tv_nsec;
  stx->stx_rdev_major = k_major(ks->st_rdev);
  stx->stx_rdev_minor = k_minor(ks->st_rdev);
  stx->stx_dev_major = k_major(ks->st_dev);
  stx->stx_dev_minor = k_minor(ks->st_dev);
}

// statx → kstat narrowing (for legacy SYS_STAT/SYS_FSTAT/SYS_NEWFSTATAT). A
// lossless round-trip for this system's value ranges.
static void kstat_from_statx(struct kstat *ks, const struct statx *stx) {
  __memset(ks, 0, sizeof(*ks));
  ks->st_dev = k_makedev(stx->stx_dev_major, stx->stx_dev_minor);
  ks->st_ino = stx->stx_ino;
  ks->st_nlink = stx->stx_nlink;
  ks->st_mode = stx->stx_mode;
  ks->st_uid = stx->stx_uid;
  ks->st_gid = stx->stx_gid;
  ks->st_rdev = k_makedev(stx->stx_rdev_major, stx->stx_rdev_minor);
  ks->st_size = (int64_t)stx->stx_size;
  ks->st_blksize = (int64_t)stx->stx_blksize;
  ks->st_blocks = (int64_t)stx->stx_blocks;
  ks->st_atim.tv_sec = stx->stx_atime.tv_sec;
  ks->st_atim.tv_nsec = stx->stx_atime.tv_nsec;
  ks->st_mtim.tv_sec = stx->stx_mtime.tv_sec;
  ks->st_mtim.tv_nsec = stx->stx_mtime.tv_nsec;
  ks->st_ctim.tv_sec = stx->stx_ctime.tv_sec;
  ks->st_ctim.tv_nsec = stx->stx_ctime.tv_nsec;
}

// per-fd-type kstat fill (the original sys_fstat switch body). FD_REGULAR/
// FD_DIR/FD_DEV delegate to the inode getattr to report the real fields, and
// fall back to basic fields without getattr; the remaining fd types
// (pipe/tty/shm) have no inode and hardcode st_mode by type.
static int fstat_fill(struct file *f, struct kstat *ks) {
  __memset(ks, 0, sizeof(*ks));
  ks->st_nlink = 1;
  ks->st_blksize = 512;
  switch (f->type) {
  case FD_REGULAR:
  case FD_DIR: {
    struct inode *ip = f->inode;
    if (!ip)
      return -EBADF;
    if (ip->i_op && ip->i_op->getattr) {
      ip->i_op->getattr(ip, ks);
    } else {
      ks->st_ino = ip->ino;
      ks->st_mode = ip->mode;
      ks->st_uid = ip->uid;
      ks->st_gid = ip->gid;
      ks->st_size = (int64_t)ip->size;
      ks->st_nlink = (uint64_t)ip->nlink;
    }
    return 0;
  }
  case FD_DEV: {
    struct inode *ip = f->inode;
    if (!ip)
      return -EBADF;
    if (ip->i_op && ip->i_op->getattr) {
      ip->i_op->getattr(ip, ks);
    } else {
      ks->st_ino = ip->ino;
      ks->st_uid = ip->uid;
      ks->st_gid = ip->gid;
      struct dev_ops *ops = (struct dev_ops *)ip->i_priv;
      if (ops && __strcmp(ops->subsystem, "drm") == 0)
        ks->st_rdev = k_makedev(DRM_MAJOR_FOR_STAT, ops->minor);
      else
        ks->st_rdev = ip->ino;
      if (ops && ops->is_block)
        ks->st_mode = S_IFBLK | 0666;
      else
        ks->st_mode = S_IFCHR | 0666;
    }
    return 0;
  }
  case FD_PIPE:
    ks->st_mode = S_IFIFO | 0644;
    return 0;
  case FD_TTY: {
    // A tty fd's f->inode is the devtmpfs /dev/pts/N node (bound by sys_open at
    // open time). musl ttyname_r uses the (dev,ino) cross-check of stat(path)
    // vs fstat(fd) to confirm the /proc/self/fd/N link target is that fd — so
    // fstat must backfill st_ino/st_rdev consistent with stat, else the closure
    // misjudges ENODEV (procfs.md §3.4.1).
    struct inode *ip = f->inode;
    if (ip) {
      ks->st_ino = ip->ino;
      ks->st_rdev = ip->ino; // char device rdev = ino (devtmpfs convention, see
                             // FD_DEV)
      ks->st_uid = ip->uid;
      ks->st_gid = ip->gid;
    }
    ks->st_mode = S_IFCHR | 0666;
    return 0;
  }
  case FD_SHM:
    ks->st_mode = S_IFREG | 0666;
    return 0;
  case FD_DRM_PRIME: {
    uint64_t size = drm_prime_object_size(f->drm_prime);
    uint64_t id = drm_prime_object_id(f->drm_prime);
    if (!size || !id || size > INT64_MAX)
      return -EBADF;
    ks->st_mode = S_IFREG | 0600;
    ks->st_ino = id;
    ks->st_size = (int64_t)size;
    ks->st_blksize = PAGE_SIZE;
    ks->st_blocks = (int64_t)(size / 512 + (size % 512 != 0));
    return 0;
  }
  case FD_DMA_BUF: {
    uint64_t size = dma_buf_size(f->dma_buf);
    uint64_t id = dma_buf_id(f->dma_buf);
    if (!size || !id || size > INT64_MAX)
      return -EBADF;
    ks->st_mode = S_IFREG | 0600;
    ks->st_ino = id;
    ks->st_size = (int64_t)size;
    ks->st_blksize = PAGE_SIZE;
    ks->st_blocks = (int64_t)(size / 512 + (size % 512 != 0));
    return 0;
  }
  default:
    return -EBADF;
  }
}

// fd path fill: AT_EMPTY_PATH + empty path → stat the fd itself.
static int vfs_fstat_fd(int fd, struct kstat *ks) {
  xtask *proc = current_task;
  if (fd < 0 || fd >= MAX_FD)
    return -EBADF;
  rcu_read_lock();
  struct file *f = fd_lookup(proc->proc->files, fd);
  if (!f) {
    rcu_read_unlock();
    return -EBADF;
  }
  file_get(f);
  rcu_read_unlock();
  int rc = fstat_fill(f, ks);
  file_put(f);
  return rc;
}

int vfs_statx(int dirfd, const char *kpath, unsigned flags, struct statx *stx) {
  // Linux do_statx: FORCE_SYNC|DONT_SYNC set together is illegal; unknown flag
  // bits are illegal.
  if ((flags & AT_STATX_SYNC_TYPE) == AT_STATX_SYNC_TYPE)
    return -EINVAL;
  if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT | AT_EMPTY_PATH |
                AT_STATX_SYNC_TYPE))
    return -EINVAL;

  struct kstat ks;
  struct inode *ip = NULL;
  int rc;

  if (kpath[0] == '\0') {
    // Empty path is only legal under AT_EMPTY_PATH (stat the fd itself), else
    // ENOENT.
    if (!(flags & AT_EMPTY_PATH))
      return -ENOENT;
    rc = vfs_fstat_fd(dirfd, &ks);
  } else {
    char relpath[256];
    if (kpath[0] == '/') {
      // Absolute path: dirfd ignored, longest-prefix match on mount table.
      char norm[256];
      if (normalize_path(kpath, norm, sizeof(norm)) < 0)
        return -ENAMETOOLONG;
      struct mount_entry *m = vfs_resolve(norm, relpath, sizeof(relpath));
      if (!m)
        return -ENOENT;
      ip = path_walk(m, relpath); // +1
    } else {
      // Relative path: resolve from dirfd (AT_FDCWD ≡ root, kernel has no CWD).
      if (normalize_path(kpath, relpath, sizeof(relpath)) < 0)
        return -ENAMETOOLONG;
      struct inode *start = resolve_dirfd_start(dirfd);
      if (IS_ERR(start))
        return (int)PTR_ERR(start);
      ip = path_walk_from(start, relpath); // +1
      inode_put(start);
    }
    if (!ip)
      return -ENOENT;
    // §3.3.4 final-segment symlink following: follow a final-segment LNK unless
    // AT_SYMLINK_NOFOLLOW is set (stat follows by default; lstat sets NOFOLLOW
    // to take the link itself). Middle segments were followed by path_walk;
    // this handles the final segment. depth guards against target loops →
    // ELOOP.
    if (ip->type == INODE_LNK && !(flags & AT_SYMLINK_NOFOLLOW)) {
      int sym_depth = 0;
      struct inode *resolved = follow_symlink(ip, &sym_depth);
      inode_put(ip);
      ip = resolved;
      if (IS_ERR(ip))
        return (int)PTR_ERR(ip);
    }
    rc = -ENOSYS;
    if (ip->i_op && ip->i_op->getattr)
      rc = ip->i_op->getattr(ip, &ks);
    inode_put(ip);
  }
  if (rc)
    return rc;
  statx_from_kstat(stx, &ks);
  return 0;
}

// sys_statx(dirfd, path, flags, mask, buf) — SYS_STATX
int64_t sys_statx(int64_t dirfd, int64_t path, int64_t flags, int64_t mask,
                  int64_t buf, int64_t unused) {
  (void)unused;
  (void)mask; // request mask is only advisory — always backfill
              // STATX_BASIC_STATS
  const char __user *upath = (const char __user *__force)path;
  if (!upath)
    return (int64_t)-EFAULT;
  char kpath[256];
  if (strncpy_from_user(kpath, upath, sizeof(kpath)) < 0)
    return (int64_t)-EFAULT;
  struct statx stx = {0};
  int rc = vfs_statx((int)dirfd, kpath, (unsigned)flags, &stx);
  if (rc)
    return (int64_t)rc;
  if (copy_to_user((void __user *__force)buf, &stx, sizeof(stx)))
    return (int64_t)-EFAULT;
  return 0;
}

// Legacy path-stat syscalls are thin kstat views over the statx core.
static int64_t sys_stat_legacy(int64_t path, int64_t buf, unsigned flags) {
  const char __user *upath = (const char __user *__force)path;
  if (!upath)
    return (int64_t)-EFAULT;
  char kpath[256];
  if (strncpy_from_user(kpath, upath, sizeof(kpath)) < 0)
    return (int64_t)-EFAULT;
  struct statx stx = {0};
  int rc = vfs_statx(AT_FDCWD, kpath, flags, &stx);
  if (rc)
    return (int64_t)rc;
  struct kstat ks;
  kstat_from_statx(&ks, &stx);
  if (copy_to_user((void __user *__force)buf, &ks, sizeof(ks)))
    return (int64_t)-EFAULT;
  return 0;
}

// sys_stat(path, stat_buf) — SYS_STAT: follow the final symlink.
int64_t sys_stat(int64_t arg1, int64_t arg2, int64_t unused1, int64_t unused2,
                 int64_t unused3, int64_t unused4) {
  (void)unused1;
  (void)unused2;
  (void)unused3;
  (void)unused4;
  return sys_stat_legacy(arg1, arg2, 0);
}

// sys_lstat(path, stat_buf) — SYS_LSTAT: inspect the final symlink itself.
int64_t sys_lstat(int64_t arg1, int64_t arg2, int64_t unused1, int64_t unused2,
                  int64_t unused3, int64_t unused4) {
  (void)unused1;
  (void)unused2;
  (void)unused3;
  (void)unused4;
  return sys_stat_legacy(arg1, arg2, AT_SYMLINK_NOFOLLOW);
}

// S07: resolve a *at dirfd to its starting directory inode (+1, caller puts),
// or ERR_PTR(-errno). AT_FDCWD resolves to the process cwd's inode: musl's *at
// wrappers (openat/unlinkat/renameat/mkdirat/fstatat/faccessat) pass AT_FDCWD
// straight to the syscall, so the kernel must honor a prior chdir. bp->cwd is
// the single source of truth (maintained by sys_chdir/sys_fchdir); it is always
// absolute, so normalize (in case of a trailing slash / '.' / '..') and resolve
// to its inode. A real dirfd must reference an open directory (ENOTDIR
// otherwise). Absolute paths are handled by the caller (fall back to
// sys_open/sys_stat/etc) before calling this.
struct inode *resolve_dirfd_start(int dirfd) {
  if (dirfd == AT_FDCWD) {
    proc *bp = current_proc;
    char norm[256];
    if (normalize_path(bp->cwd, norm, sizeof(norm)) < 0)
      return ERR_PTR(-ENAMETOOLONG);
    struct inode *ip = vfs_open_kern(norm); // +1 or NULL
    // cwd is only ever set by sys_chdir/sys_fchdir after a S_ISDIR check, so a
    // valid cwd resolves to a directory; NULL (cwd removed out from under us)
    // → ENOENT, matching Linux.
    if (!ip)
      return ERR_PTR(-ENOENT);
    return ip;
  }
  if (dirfd < 0)
    return ERR_PTR(-EBADF);
  xtask *proc = current_task;
  rcu_read_lock();
  struct file *f = fd_lookup(proc->proc->files, dirfd);
  if (!f) {
    rcu_read_unlock();
    return ERR_PTR(-EBADF);
  }
  file_get(f);
  rcu_read_unlock();
  struct inode *ip = NULL;
  if (!f->inode)
    ip = ERR_PTR(-EBADF);
  else if (f->inode->type != INODE_DIR)
    ip = ERR_PTR(-ENOTDIR);
  else
    ip = inode_get(f->inode); // +1
  file_put(f);
  return ip;
}

// openat(dirfd, path, flags, mode). Absolute path → sys_open (mount-table
// match, unchanged). Relative path → resolve from dirfd's directory inode via
// path_walk_from/path_walk_parent_from. AT_FDCWD → from the process cwd
// (resolve_dirfd_start resolves bp->cwd to its inode).
int64_t sys_openat(int64_t dirfd, int64_t path, int64_t flags, int64_t mode,
                   int64_t unused1, int64_t unused2) {
  (void)unused1;
  (void)unused2;
  const char __user *upath = (const char __user *__force)path;
  if (!upath)
    return (int64_t)-EFAULT;

  char kpath[256];
  long n = strncpy_from_user(kpath, upath, sizeof(kpath));
  if (n < 0)
    return (int64_t)-EFAULT;

  // Absolute path: dirfd ignored, resolve via mount table (existing path).
  if (kpath[0] == '/')
    return sys_open(path, flags, mode, 0, 0, 0);

  // Relative path: resolve start inode from dirfd (or root for AT_FDCWD).
  struct inode *start = resolve_dirfd_start((int)dirfd);
  if (IS_ERR(start))
    return (int64_t)PTR_ERR(start);

  char relpath[256];
  if (normalize_path(kpath, relpath, sizeof(relpath)) < 0) {
    inode_put(start);
    return (int64_t)-ENAMETOOLONG;
  }

  int iflags = (int)flags;
  struct mount_entry *start_mount = mount_of_inode(start);
  if (start_mount && (start_mount->m_flags & MS_RDONLY) &&
      (iflags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC))) {
    inode_put(start);
    return (int64_t)-EROFS;
  }
  struct inode *ip = path_walk_from(start, relpath); // +1 or NULL
  if (ip) {
    if ((iflags & O_CREAT) && (iflags & O_EXCL)) {
      inode_put(ip);
      inode_put(start);
      return (int64_t)-EEXIST;
    }
    if ((iflags & O_TRUNC) && ip->type == INODE_REGULAR && ip->size > 0) {
      if (!ip->i_op || !ip->i_op->setattr) {
        inode_put(ip);
        inode_put(start);
        return (int64_t)-EPERM;
      }
      ip->i_op->setattr(ip, 0);
    }
  } else if (iflags & O_CREAT) {
    char lastname[256];
    struct inode *parent = NULL;
    int rc = path_walk_parent_from(start, relpath, &parent, lastname,
                                   sizeof(lastname));
    if (rc) {
      if (parent)
        inode_put(parent);
      inode_put(start);
      return (int64_t)rc;
    }
    if (!parent->i_op || !parent->i_op->create) {
      inode_put(parent);
      inode_put(start);
      return (int64_t)-EACCES;
    }
    int eff_mode = (int)mode & 0777;
    eff_mode = eff_mode & ~(int)current_proc->umask;
    ip = parent->i_op->create(parent, lastname, eff_mode); // +1
    inode_put(parent);
    if (IS_ERR(ip)) {
      inode_put(start);
      return PTR_ERR(ip);
    }
    if (!ip) {
      inode_put(start);
      return (int64_t)-ENOMEM;
    }
    ip->mode = (ip->mode & ~0777) | (uint32_t)eff_mode;
    ip->uid = current_proc->uid;
    ip->gid = current_proc->gid;
  } else {
    inode_put(start);
    return (int64_t)-ENOENT;
  }
  inode_put(start);
  // ip is +1 (from path_walk_from or create).
  ip->mount = mount_of_inode(ip); // lazy, mirrors sys_open

  if (ip->type == INODE_DIR &&
      (iflags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC))) {
    inode_put(ip);
    return (int64_t)-EISDIR;
  }
  // O_DIRECTORY: caller requires a directory; non-dir → ENOTDIR (Linux).
  if ((iflags & O_DIRECTORY) && ip->type != INODE_DIR) {
    inode_put(ip);
    return (int64_t)-ENOTDIR;
  }
  if (ip->type == INODE_SOCKET) {
    inode_put(ip);
    return (int64_t)-ENXIO;
  }

  xtask *proc = current_task;
  files *files = proc->proc->files;
  spinlock *fdlk = &files->fd_lock;
  spin_lock(fdlk);
  int fd = alloc_fd(files, 0);
  if (fd < 0) {
    spin_unlock(fdlk);
    inode_put(ip);
    return (int64_t)-EMFILE;
  }
  struct file *f = (struct file *)kmalloc(sizeof(struct file));
  if (!f) {
    spin_unlock(fdlk);
    inode_put(ip);
    return (int64_t)-ENOMEM;
  }
  __memset(f, 0, sizeof(*f));
  refcount_set(&f->f_count, 1);
  f->f_op = ip->i_fop;
  f->mount = start_mount;
  if (start_mount)
    atomic_inc(&start_mount->sb.active_files);
  if (ip->type == INODE_DIR) {
    f->type = FD_DIR;
    f->flags = O_RDONLY;
    f->inode = ip;
    f->offset = 0;
  } else {
    f->type = FD_REGULAR;
    f->flags = iflags & (O_RDONLY | O_WRONLY | O_RDWR | O_APPEND | O_NONBLOCK);
    f->inode = ip;
    f->offset = 0;
  }
  fd_install(files, fd, f);
  fd_set_cloexec(files, fd, (iflags & O_CLOEXEC) ? 1 : 0);
  spin_unlock(fdlk);
  return (int64_t)fd;
}

// newfstatat(dirfd, path, buf, flags) — thin vfs_statx wrapper (narrowing to
// kstat). AT_EMPTY_PATH + empty path → stat dirfd itself; AT_SYMLINK_NOFOLLOW
// accepted but no-op (no symlink).
int64_t sys_newfstatat(int64_t dirfd, int64_t path, int64_t buf, int64_t flags,
                       int64_t unused1, int64_t unused2) {
  (void)unused1;
  (void)unused2;
  const char __user *upath = (const char __user *__force)path;
  if (!upath)
    return (int64_t)-EFAULT;
  char kpath[256];
  if (strncpy_from_user(kpath, upath, sizeof(kpath)) < 0)
    return (int64_t)-EFAULT;
  struct statx stx = {0};
  int rc = vfs_statx((int)dirfd, kpath, (unsigned)flags, &stx);
  if (rc)
    return (int64_t)rc;
  struct kstat ks;
  kstat_from_statx(&ks, &stx);
  if (copy_to_user((void __user *__force)buf, &ks, sizeof(ks)))
    return (int64_t)-EFAULT;
  return 0;
}

// sys_fstat(fd, stat_buf) — SYS_FSTAT: thin vfs_statx wrapper (AT_EMPTY_PATH
// path, narrowing to kstat).
int64_t sys_fstat(int64_t arg1, int64_t arg2, int64_t unused1, int64_t unused2,
                  int64_t unused3, int64_t unused4) {
  (void)unused1;
  (void)unused2;
  (void)unused3;
  (void)unused4;
  struct statx stx = {0};
  int rc = vfs_statx((int)arg1, "", AT_EMPTY_PATH, &stx);
  if (rc)
    return (int64_t)rc;
  struct kstat ks;
  kstat_from_statx(&ks, &stx);
  if (copy_to_user((void __user *__force)arg2, &ks, sizeof(ks)))
    return (int64_t)-EFAULT;
  return 0;
}

// sys_truncate(path, len) — SYS_TRUNCATE (group 3)
// Resolve the path to an inode via mount framework + path_walk, then
// dispatch size change to i_op->setattr (eliminates raw fat32_open).
int64_t sys_truncate(int64_t arg1, int64_t arg2, int64_t unused1,
                     int64_t unused2, int64_t unused3, int64_t unused4) {
  const char __user *upath = (const char __user *__force)arg1;
  int64_t len = arg2;
  if (!upath)
    return (int64_t)-EFAULT;
  if (len < 0)
    return (int64_t)-EINVAL;
  char relpath[256];
  struct mount_entry *m = vfs_resolve_user(upath, relpath, sizeof(relpath));
  if (IS_ERR(m))
    return PTR_ERR(m);
  if (!m)
    return (int64_t)-ENOENT;
  struct inode *ip = path_walk(m, relpath); // +1
  if (!ip)
    return (int64_t)-ENOENT;
  if (ip->type != INODE_REGULAR) {
    inode_put(ip);
    return (int64_t)-EISDIR;
  }
  if (!ip->i_op || !ip->i_op->setattr) {
    inode_put(ip);
    return (int64_t)-EPERM; // matches Linux notify_change: no setattr → EPERM
  }
  int rc = ip->i_op->setattr(ip, (uint64_t)len); // lock held internally by
                                                 // setattr (§6.6)
  inode_put(ip);
  return (int64_t)rc;
}

// sys_fsync(fd) — SYS_FSYNC (group 3): write back dirty pages of one inode.
int64_t sys_fsync(int64_t arg1, int64_t datasync_arg, int64_t unused2,
                  int64_t unused3, int64_t unused4, int64_t unused5) {
  int fd = (int)arg1;
  xtask *proc = current_task;
  if (fd < 0 || fd >= MAX_FD)
    return (int64_t)-EBADF;

  rcu_read_lock();
  struct file *f = fd_lookup(proc->proc->files, fd);
  if (!f || (f->type != FD_REGULAR && f->type != FD_DIR)) {
    rcu_read_unlock();
    return (int64_t)-EINVAL;
  }
  file_get(f);
  struct inode *ip = f->inode;
  rcu_read_unlock();
  if (!ip) {
    file_put(f);
    return (int64_t)-EBADF;
  }

  int rc = page_cache_flush_inode(ip);
  if (!rc && f->f_op && f->f_op->fsync)
    rc = f->f_op->fsync(f, datasync_arg != 0);
  if (!rc && ip->i_sb && ip->i_sb->part)
    rc = partition_flush(ip->i_sb->part);
  file_put(f);
  return (int64_t)rc;
}

// sys_sync() — SYS_SYNC (group 3): write back all dirty pages.
int64_t sys_sync(int64_t unused1, int64_t unused2, int64_t unused3,
                 int64_t unused4, int64_t unused5, int64_t unused6) {
  (void)unused1;
  (void)unused2;
  (void)unused3;
  (void)unused4;
  (void)unused5;
  (void)unused6;
  int rc = page_cache_flush_all();
  if (!rc)
    rc = mount_sync_all();
  int flush_rc = block_flush(block_primary_device());
  return (int64_t)(rc ? rc : flush_rc);
}

// sys_mkdir(path, mode) — SYS_MKDIR
int64_t sys_mkdir(int64_t arg1, int64_t arg2, int64_t unused1, int64_t unused2,
                  int64_t unused3, int64_t unused4) {
  const char __user *upath = (const char __user *__force)arg1;

  if (!upath)
    return (int64_t)-EFAULT;
  char relpath[256], lastname[256];
  struct mount_entry *m = vfs_resolve_user(upath, relpath, sizeof(relpath));
  if (IS_ERR(m))
    return PTR_ERR(m);
  if (!m)
    return (int64_t)-ENOENT;
  struct inode *parent = NULL;
  int rc = path_walk_parent(m, relpath, &parent, lastname, sizeof(lastname));
  if (rc) {
    if (parent)
      inode_put(parent);
    return (int64_t)rc;
  }
  if (!parent->i_op || !parent->i_op->mkdir) {
    inode_put(parent);
    return (int64_t)-EPERM; // matches Linux vfs_mkdir: no mkdir → EPERM
  }
  // First check whether the target already exists; fat32_dir_mkdir does not
  // check for duplicates (it directly allocates a cluster to create an entry),
  // and without this layer it would create duplicate same-name directory
  // entries (see the duplicate /var bug). Matches Linux vfs_mkdir's
  // lookup_one_len: hitting an existing entry → EEXIST.
  struct inode *existing = path_walk(m, relpath); // +1
  if (existing) {
    inode_put(existing);
    inode_put(parent);
    return (int64_t)-EEXIST;
  }
  int eff_mode = ((int)arg2 & 0777) & ~(int)current_proc->umask;
  rc = parent->i_op->mkdir(parent, lastname, eff_mode);
  inode_put(parent);
  if (rc != 0)
    return (int64_t)rc;
  // S08: mkdir returns no inode; re-fetch the new directory to set
  // owner = creating process + apply the umask permission bits (keep the
  // S_IFDIR directory type bit).
  struct inode *nip = path_walk(m, relpath); // +1
  if (nip) {
    nip->mode = (nip->mode & ~0777) | (uint32_t)eff_mode;
    nip->uid = current_proc->uid;
    nip->gid = current_proc->gid;
    inode_put(nip);
  }
  return 0;
}

// sys_mknod(path, mode, dev) — SYS_MKNOD
// Matches Linux mknod: create a mode-type node in path's parent dir.
// tmpfs supports S_IFREG/S_IFIFO/S_IFSOCK (create returns 0);
// S_IFCHR/S_IFBLK/S_IFDIR (device nodes belong to devtmpfs, directories use
// mkdir) → -EOPNOTSUPP.
int64_t sys_mknod(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                  int64_t unused2, int64_t unused3) {
  (void)unused1;
  (void)unused2;
  (void)unused3;
  const char __user *upath = (const char __user *__force)arg1;
  int mode = (int)arg2;
  (void)arg3; // dev only matters for CHR/BLK; the supported types don't use it
              // here, ignore

  if (!upath)
    return (int64_t)-EFAULT;
  int fmt = mode & S_IFMT;
  if (fmt != S_IFREG && fmt != S_IFIFO && fmt != S_IFSOCK)
    return (int64_t)-EOPNOTSUPP; // matches Linux: tmpfs doesn't build
                                 // device/directory nodes

  char relpath[256], lastname[256];
  struct mount_entry *m = vfs_resolve_user(upath, relpath, sizeof(relpath));
  if (IS_ERR(m))
    return PTR_ERR(m);
  if (!m)
    return (int64_t)-ENOENT;

  struct inode *parent = NULL;
  int rc = path_walk_parent(m, relpath, &parent, lastname, sizeof(lastname));
  if (rc) {
    if (parent)
      inode_put(parent);
    return (int64_t)rc;
  }

  if (!parent->i_op || !parent->i_op->create) {
    inode_put(parent);
    return (int64_t)-EPERM;
  }
  // S08: permission bits apply umask, type bits keep; set owner = creating
  // process.
  int eff_mode = (mode & S_IFMT) | ((mode & 0777) & ~(int)current_proc->umask);
  struct inode *ip = parent->i_op->create(parent, lastname, eff_mode);
  inode_put(parent);
  if (IS_ERR(ip))
    return PTR_ERR(ip);
  if (!ip)
    return (int64_t)-ENOMEM;
  ip->uid = current_proc->uid;
  ip->gid = current_proc->gid;
  inode_put(ip); // create already did the initial +1 in inode_create; balance
  return 0;
}

// sys_unlink(path) — SYS_UNLINK
int64_t sys_unlink(int64_t arg1, int64_t unused1, int64_t unused2,
                   int64_t unused3, int64_t unused4, int64_t unused5) {
  const char __user *upath = (const char __user *__force)arg1;

  if (!upath)
    return (int64_t)-EFAULT;
  char relpath[256], lastname[256];
  struct mount_entry *m = vfs_resolve_user(upath, relpath, sizeof(relpath));
  if (IS_ERR(m))
    return PTR_ERR(m);
  if (!m)
    return (int64_t)-ENOENT;
  struct inode *parent = NULL;
  int rc = path_walk_parent(m, relpath, &parent, lastname, sizeof(lastname));
  if (rc) {
    if (parent)
      inode_put(parent);
    return (int64_t)rc;
  }
  if (!parent->i_op || !parent->i_op->unlink) {
    inode_put(parent);
    return (int64_t)-EPERM; // matches Linux vfs_unlink: no unlink → EPERM
  }
  rc = parent->i_op->unlink(parent, lastname);
  inode_put(parent);
  return (int64_t)rc;
}

// sys_rename(oldpath, newpath) — SYS_RENAME
// Following the sys_unlink template: two path_walk_parent calls take the two
// parents + lastnames, then call old_parent->i_op->rename. Cross-mount not
// supported (vfs_resolve strips mount-point prefixes; relpath is confined to a
// single mount), the db scenario is all within the single tmpfs mount
// /run/udev/data/.
int64_t sys_rename(int64_t arg1, int64_t arg2, int64_t unused1, int64_t unused2,
                   int64_t unused3, int64_t unused4) {
  (void)unused1;
  (void)unused2;
  (void)unused3;
  (void)unused4;
  const char __user *uold = (const char __user *__force)arg1;
  const char __user *unew = (const char __user *__force)arg2;
  if (!uold || !unew)
    return (int64_t)-EFAULT;

  char old_rel[256], old_name[256];
  char new_rel[256], new_name[256];

  struct mount_entry *old_m = vfs_resolve_user(uold, old_rel, sizeof(old_rel));
  if (IS_ERR(old_m))
    return (int64_t)PTR_ERR(old_m);
  if (!old_m)
    return (int64_t)-ENOENT;
  struct mount_entry *new_m = vfs_resolve_user(unew, new_rel, sizeof(new_rel));
  if (IS_ERR(new_m))
    return (int64_t)PTR_ERR(new_m);
  if (!new_m)
    return (int64_t)-ENOENT;

  // The db scenario has old/new in the same mount; a cross-mount returns
  // -EXDEV (matching Linux rename(2)).
  if (old_m != new_m)
    return (int64_t)-EXDEV;

  struct inode *old_parent = NULL, *new_parent = NULL;
  int rc =
      path_walk_parent(old_m, old_rel, &old_parent, old_name, sizeof(old_name));
  if (rc) {
    if (old_parent)
      inode_put(old_parent);
    return (int64_t)rc;
  }
  rc =
      path_walk_parent(new_m, new_rel, &new_parent, new_name, sizeof(new_name));
  if (rc) {
    if (new_parent)
      inode_put(new_parent);
    inode_put(old_parent);
    return (int64_t)rc;
  }

  if (!old_parent->i_op || !old_parent->i_op->rename) {
    inode_put(old_parent);
    inode_put(new_parent);
    return (int64_t)-EPERM; // matches Linux vfs_rename: no rename → EPERM
  }
  rc = old_parent->i_op->rename(old_parent, old_name, new_parent, new_name);
  inode_put(old_parent);
  inode_put(new_parent);
  return (int64_t)rc;
}

// sys_rmdir(path) — SYS_RMDIR
int64_t sys_rmdir(int64_t arg1, int64_t unused1, int64_t unused2,
                  int64_t unused3, int64_t unused4, int64_t unused5) {
  const char __user *upath = (const char __user *__force)arg1;

  if (!upath)
    return (int64_t)-EFAULT;
  char relpath[256], lastname[256];
  struct mount_entry *m = vfs_resolve_user(upath, relpath, sizeof(relpath));
  if (IS_ERR(m))
    return PTR_ERR(m);
  if (!m)
    return (int64_t)-ENOENT;
  struct inode *parent = NULL;
  int rc = path_walk_parent(m, relpath, &parent, lastname, sizeof(lastname));
  if (rc) {
    if (parent)
      inode_put(parent);
    return (int64_t)rc;
  }
  if (!parent->i_op || !parent->i_op->rmdir) {
    inode_put(parent);
    return (int64_t)-EPERM; // matches Linux vfs_rmdir: no rmdir → EPERM
  }
  rc = parent->i_op->rmdir(parent, lastname);
  inode_put(parent);
  return (int64_t)rc;
}

// sys_dev_create(name, shm_fd, minor) — SYS_DEV_CREATE
// Kernel auto-fills driver_pid=current_task->pid, is_block=false, callbacks
// NULL (user-space driver). minor stored in dev_ops for ioctl req routing.
int64_t sys_dev_create(int64_t arg1, int64_t arg2, int64_t arg3,
                       int64_t unused1, int64_t unused2, int64_t unused3) {
  const char __user *uname = (const char __user *__force)arg1;
  int shm_fd = (int)arg2;
  uint32_t minor = (uint32_t)arg3;
  struct shm *dev_shm = NULL;

  if (!uname)
    return (int64_t)-EFAULT;
  char name[32];
  if (strncpy_from_user(name, uname, 32) < 0)
    return (int64_t)-EFAULT;

  struct dev_ops *kops = kmalloc(sizeof(struct dev_ops));
  if (!kops)
    return (int64_t)-ENOMEM;
  __memset(kops, 0, sizeof(struct dev_ops));
  kops->storage = DEV_OPS_HEAP;

  // Force driver_pid to current process — user-space can't set this
  kops->driver_pid = current_task->pid;
  kops->is_block = false;
  kops->minor = minor;
  // All callbacks remain NULL for user-space drivers (IPC proxy handles
  // requests)

  // Resolve shm_fd to struct shm* (if provided)
  if (shm_fd >= 0) {
    xtask *proc = current_task;
    if (shm_fd >= MAX_FD) {
      kfree(kops);
      return (int64_t)-EBADF;
    }
    spinlock *fdlk = &proc->proc->files->fd_lock;
    spin_lock(fdlk);
    struct file *sf = proc->proc->files->fd_table[shm_fd];
    if (!sf || sf->type != FD_SHM) {
      spin_unlock(fdlk);
      kfree(kops);
      return (int64_t)-EINVAL;
    }
    dev_shm = sf->shm;
    spin_unlock(fdlk);
  }

  int rc = devtmpfs_create(name, kops, dev_shm);
  if (rc != 0) {
    kfree(kops);
    return rc;
  }

  return 0;
}

// sys_getdents(fd, buf, len) — SYS_GETDENTS64
// Read directory entries into user buffer.
// fd must be FD_DIR. Returns bytes written, 0 on EOF, or negative errno.
int64_t sys_getdents(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                     int64_t unused2, int64_t unused3) {
  int fd = (int)arg1;
  void __user *buf = (void __user *__force)arg2;
  size_t len = (size_t)arg3;

  if (fd < 0 || fd >= MAX_FD)
    return (int64_t)-EINVAL;
  if (len == 0 || len > (size_t)1048576)
    return (int64_t)-EINVAL;

  rcu_read_lock();
  struct file *f = fd_lookup(current_proc->files, fd);
  if (!f || f->type != FD_DIR) {
    rcu_read_unlock();
    return (int64_t)-ENOTDIR;
  }
  file_get(f);
  rcu_read_unlock();

  struct inode *ip = f->inode;
  if (!ip) {
    file_put(f);
    return (int64_t)-EBADF;
  }

  void *kbuf = kmalloc(len);
  if (!kbuf) {
    file_put(f);
    return (int64_t)-ENOMEM;
  }
  struct mount_entry *m = mount_of_inode(ip);
  if (!m || !m->fs->getdents) {
    kfree(kbuf);
    file_put(f);
    return (int64_t)-ENOTDIR;
  }
  struct dir_context ctx = {
      .pos = f->offset,
      .buf = kbuf,
      .len = len,
      .written = 0,
  };
  ssize_t ret = m->fs->getdents(ip, &ctx);
  if (ret < 0) {
    kfree(kbuf);
    file_put(f);
    return (int64_t)ret;
  }
  f->offset = ctx.pos;
  if (copy_to_user(buf, kbuf, ctx.written)) {
    kfree(kbuf);
    file_put(f);
    return (int64_t)-EFAULT;
  }
  kfree(kbuf);
  file_put(f);
  return (int64_t)ctx.written;
}

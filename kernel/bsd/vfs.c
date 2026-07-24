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
#include "kernel/bsd/inode.h"
#include "kernel/bsd/mount.h"
#include "kernel/bsd/page_cache.h"
#include "kernel/bsd/proc.h"
#include "kernel/bsd/pty.h"
#include "kernel/bsd/syscall.h"
#include "kernel/bsd/sysfs.h"
#include "kernel/bsd/tmpfs.h"
#include "kernel/bsd/types.h"
#include "kernel/driver/ahci.h"
#include "kernel/driver/blk_dev.h"
#include "kernel/driver/serial.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/kasan.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/rcu.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/xtask.h"
#include <stdbool.h>
#include <stddef.h>
#include <xos/errno.h>
#include <xos/fcntl.h>
#include <xos/stat.h>

void vfs_init(void) {
  // inode_init, page_cache_init, devtmpfs_init are called in kernel_main before
  // driver_init. drm_dev_register() is called from virtio_gpu_init
  // (driver_init).
  mount_init();
  serial_dev_register();
  pty_init();

  /* Try FAT32 on each AHCI port. disk.img is a single disk now (two
     partitions: ESP + root), so the root FAT32 is on port 0 — but we
     keep the multi-port scan for robustness. */
  int try_ports[] = {0, 1, 2, 3, 4, 5};
  int rc = -1;
  for (int pi = 0; pi < 6; pi++) {
    if (ahci_set_active_port(try_ports[pi]) != 0)
      continue;
    rc = fat32_init();
    if (rc == 0) {
      printk(LOG_INFO, "vfs_init: FAT32 inited on port %d\n", try_ports[pi]);
      register_fstype(&fat32_fstype);
      register_fstype(&devtmpfs_fstype);
      register_fstype(&sysfs_fstype);
      register_fstype(&tmpfs_fstype);
      sysfs_init();
      mount_internal(&fat32_fstype, "/", NULL);
      /* Create /dev directory entry on FAT32 root so getdents("/") sees it.
       * fat32_mkdir is not idempotent (it allocates a cluster unconditionally),
       * so only create when the entry is missing. */
      {
        uint8_t ksb[256];
        if (fat32_stat("/dev", ksb) != 0)
          fat32_mkdir("/dev");
      }
      mount_internal(&devtmpfs_fstype, "/dev", NULL);
      /* Create /sys directory on FAT32 root for getdents("/") visibility */
      {
        uint8_t ksb[256];
        if (fat32_stat("/sys", ksb) != 0)
          fat32_mkdir("/sys");
      }
      mount_internal(&sysfs_fstype, "/sys", sysfs_root_node());
      /* Create /run directory on FAT32 root for getdents("/") visibility,
       * then mount tmpfs on /run (内存 fs，udevd db/socket 前置)。 */
      {
        uint8_t ksb[256];
        if (fat32_stat("/run", ksb) != 0)
          fat32_mkdir("/run");
      }
      mount_internal(&tmpfs_fstype, "/run", NULL);
      devtmpfs_create("sda", &blk_dev_ops, NULL);
      break;
    }
  }
  if (rc != 0) {
    printk(LOG_ERROR, "vfs_init: FAT32 init failed on all ports\n");
    return;
  }
}

/* path_walk:逐段 lookup 查目标 inode(已 inode_get,+1,调用者 put)。
 * relpath 始终在单个 mount 内(vfs_resolve 已剥离挂载点前缀);不做 fs 内
 * `..` 跨 mount 穿越。中间段须为 INODE_DIR,否则返 NULL。最后一段不校验类型。 */
struct inode *path_walk(struct mount_entry *m, const char *relpath) {
  if (!m->fs->mount_root)
    return NULL;
  struct inode *dir = m->fs->mount_root(m); /* 根 inode(+1) */
  if (!dir)
    return NULL;
  const char *p = relpath;
  while (*p) {
    while (*p == '/')
      p++;
    if (!*p)
      break; /* 尾部斜杠,dir 即目标 */
    const char *seg = p;
    while (*p && *p != '/')
      p++;
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
    struct inode *next = dir->i_op->lookup(dir, name); /* +1 */
    inode_put(dir); /* 释放上一段(对齐 dget/dput) */
    dir = next;
    if (!dir)
      return NULL;
  }
  return dir; /* 目标,+1,调用者 put */
}

/* path_walk_parent:走到倒数第二段,返回父目录 inode(+1,调用者 put)+
 * 最后一段名写入 lastname。空 relpath 或 "/" 显式拒。 */
int path_walk_parent(struct mount_entry *m, const char *relpath,
                     struct inode **out_parent, char *lastname,
                     size_t lastcap) {
  *out_parent = NULL;
  lastname[0] = '\0';
  if (!relpath[0] || (relpath[0] == '/' && relpath[1] == '\0'))
    return -EBUSY; /* 根无 parent、无 lastname(mkdir/rmdir "/" 对齐 -EBUSY) */
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
      /* seg 是最后一段 */
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
    struct inode *child = dir->i_op->lookup(dir, name); /* +1 */
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

/* path_walk_from:逐段 lookup,从给定 start inode 起解析 relpath(+1,调用者 put)。
 * 与 path_walk 同语义,只是起点是 dirfd 指向的目录 inode 而非 mount root。
 * relpath 不得以 '/' 开头(调用者对绝对路径退回 root 解析)。中间段须 INODE_DIR。
 */
struct inode *path_walk_from(struct inode *start, const char *relpath) {
  if (!start)
    return NULL;
  struct inode *dir = inode_get(start);
  const char *p = relpath;
  while (*p) {
    while (*p == '/')
      p++;
    if (!*p)
      break; /* 尾部斜杠,dir 即目标 */
    const char *seg = p;
    while (*p && *p != '/')
      p++;
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
    struct inode *next = dir->i_op->lookup(dir, name); /* +1 */
    inode_put(dir);
    dir = next;
    if (!dir)
      return NULL;
  }
  return dir; /* +1,调用者 put */
}

/* path_walk_parent_from:同 path_walk_parent,但起点为 start inode(+1 parent,
 * 调用者 put)+ 最后一段名。 */
int path_walk_parent_from(struct inode *start, const char *relpath,
                          struct inode **out_parent, char *lastname,
                          size_t lastcap) {
  *out_parent = NULL;
  lastname[0] = '\0';
  if (!start)
    return -ENOENT;
  if (!relpath[0] || (relpath[0] == '/' && relpath[1] == '\0'))
    return -EBUSY; /* 根无 parent、无 lastname */
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
      /* seg 是最后一段 */
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
    struct inode *child = dir->i_op->lookup(dir, name); /* +1 */
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

/* vfs_open_kern:内核态 path 解析,返 +1 inode 或 NULL(不装 fd、不做 user copy)。
 */
struct inode *vfs_open_kern(const char *kpath) {
  char relpath[256];
  struct mount_entry *m = vfs_resolve(kpath, relpath, sizeof(relpath));
  if (!m)
    return NULL;
  return path_walk(m, relpath); /* +1,调用者 put */
}

/* S19 §7: kernel-mode inode read for execve. Only regular files backed by a
 * real filesystem (fat32) are readable here; char devices / pseudo-fs / tmpfs
 * are not executable, so execve bails with -ENOEXEC before touching the inode
 * data. tmpfs kernel-read (for memfd-style tmpfs binaries) is deferred — the
 * interface stays generic so adding it later does not touch execve again. */
int vfs_read_kernel(struct inode *ip, uint64_t offset, void *buf,
                    size_t count) {
  if (!ip || !buf)
    return -EINVAL;
  if (ip->type == INODE_DIR)
    return -EISDIR;
  if (ip->type != INODE_REGULAR)
    return -ENOEXEC;
  /* fat32 is the only regular-file fs with a kernel inode-read (fat32_read);
   * it keys off ip->start_cluster, so any INODE_REGULAR it created is readable.
   * A regular inode from another fs would have no backing read here — but the
   * only regular-file fs today is fat32 (tmpfs regulars are read via f_op). */
  return fat32_read(ip, offset, buf, count);
}

/* sys_open(path, flags, mode) — SYS_OPEN */
int64_t sys_open(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                 int64_t unused2, int64_t unused3) {
  const char __user *upath = (const char __user *__force)arg1;
  int flags = (int)arg2;

  /* 1. Resolve via mount table (longest-prefix match) */
  char relpath[256];
  struct mount_entry *m = vfs_resolve_user(upath, relpath, sizeof(relpath));
  if (IS_ERR(m))
    return PTR_ERR(m);
  if (!m)
    return (int64_t)-ENOENT;

  /* 2. devtmpfs device files: delegate to devtmpfs_open so the fd is
   * created as FD_DEV and ops->open (ptmx/pts, serial, etc.) runs.
   * The bare "/dev" directory (relpath empty) falls through to the
   * generic directory path below. */
  if (m->fs == &devtmpfs_fstype && relpath[0] != '\0') {
    int64_t dev_ret = devtmpfs_open(current_task, relpath, flags, m);
    return dev_ret;
  }

  /* 3. 查已存在(逐段 path_walk) */
  struct inode *ip = path_walk(m, relpath); /* +1 */
  if (ip) {
    /* O_EXCL: file must not already exist. */
    if ((flags & O_CREAT) && (flags & O_EXCL)) {
      inode_put(ip);
      return (int64_t)-EEXIST;
    }
    /* O_TRUNC: 走 i_op->setattr(非 fat32 硬编码)。仅对 INODE_REGULAR。 */
    if ((flags & O_TRUNC) && ip->type == INODE_REGULAR && ip->size > 0) {
      if (!ip->i_op || !ip->i_op->setattr) {
        inode_put(ip);
        return (
            int64_t)-EPERM; /* 对齐 Linux notify_change:无 setattr → EPERM */
      }
      ip->i_op->setattr(ip, 0); /* 锁由 setattr 内部持(§6.6) */
    }
  } else if (flags & O_CREAT) {
    /* 不存在 + O_CREAT:path_walk_parent 拿父目录 + 最后一段名 */
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
    /* S08: 应用 umask(mode & ~umask),创建后设 owner=当前进程 uid/gid。
     * umask 在此减而非 create 内部:create 不知调用者 umask。 */
    int eff_mode = (int)arg3 & 0777;
    eff_mode = eff_mode & ~(int)current_proc->umask;
    ip = parent->i_op->create(parent, lastname, eff_mode); /* +1 新 inode */
    inode_put(parent); /* 还 path_walk_parent 的 parent */
    if (IS_ERR(ip))
      return PTR_ERR(ip);
    if (!ip)
      return (int64_t)-ENOMEM;
    /* S08: 新建文件 owner=创建进程(非硬编 0/调用者)。仅普通文件 create;
     * socket 走 vfs_mknod_socket(mknod)路径。 */
    ip->mode = (ip->mode & ~0777) | (uint32_t)eff_mode;
    ip->uid = current_proc->uid;
    ip->gid = current_proc->gid;
  } else {
    return (int64_t)-ENOENT;
  }
  /* ip 此刻 +1(来自 path_walk 或 create)。 */
  ip->mount = m; /* 惰性设 mount(§6 不变式2:仅 sys_open 设) */

  /* Reject write access to directories (POSIX EISDIR). */
  if (ip->type == INODE_DIR &&
      (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC))) {
    inode_put(ip);
    return (int64_t)-EISDIR;
  }

  /* O_DIRECTORY: caller requires a directory; non-dir → ENOTDIR (Linux). */
  if ((flags & O_DIRECTORY) && ip->type != INODE_DIR) {
    inode_put(ip);
    return (int64_t)-ENOTDIR;
  }

  /* Linux 语义：open() 一个 socket 文件返 ENXIO（任何 flags）。
   * socket 文件只能经 bind/connect 访问，不能 open 出 fd 读写。 */
  if (ip->type == INODE_SOCKET) {
    inode_put(ip);
    return (int64_t)-ENXIO;
  }

  /* 4. Allocate fd (under fd_lock) */
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

  /* 5. Allocate struct file */
  struct file *f = (struct file *)kmalloc(sizeof(struct file));
  if (!f) {
    spin_unlock(fdlk);
    inode_put(ip);
    return (int64_t)-ENOMEM;
  }
  __memset(f, 0, sizeof(*f));
  refcount_set(&f->f_count, 1);

  /* 6. Set up fd entry */
  /* sysfs 属性文件: 设 f_op = sysfs_fops */
  if (ip->type == INODE_REGULAR && ip->mount &&
      __strcmp(ip->mount->fs->name, "sysfs") == 0)
    f->f_op = &sysfs_fops;
  /* tmpfs 普通文件: 设 f_op = tmpfs_file_fops（read/write 走 tmpfs 内存
   * buffer）。 sys_read/sys_write 在 f_op 非 NULL 时优先走 fop 回调，NULL 则落
   * FD_REGULAR→ FAT32 page cache（对 tmpfs inode 错误），故 tmpfs
   * 普通文件必须挂 f_op。 */
  if (ip->type == INODE_REGULAR && ip->mount &&
      __strcmp(ip->mount->fs->name, "tmpfs") == 0)
    f->f_op = &tmpfs_file_fops;

  if (ip->type == INODE_DIR) {
    f->type = FD_DIR;
    f->flags = O_RDONLY;
    f->inode = ip;
    f->offset = 0; /* directory scan position */
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

/* sys_stat(path, stat_buf) — SYS_STAT */
int64_t sys_stat(int64_t arg1, int64_t arg2, int64_t unused1, int64_t unused2,
                 int64_t unused3, int64_t unused4) {
  const char __user *upath = (const char __user *__force)arg1;
  void __user *stat_buf = (void __user *__force)arg2;

  if (!upath)
    return (int64_t)-EFAULT;

  char relpath[256];
  struct mount_entry *m = vfs_resolve_user(upath, relpath, sizeof(relpath));
  if (IS_ERR(m))
    return PTR_ERR(m);
  if (!m)
    return (int64_t)-ENOENT;

  struct inode *ip = path_walk(m, relpath); /* +1 */
  if (!ip)
    return (int64_t)-ENOENT;
  uint8_t kstat_buf[256];
  int rc = -ENOSYS;
  if (ip->i_op && ip->i_op->getattr)
    rc = ip->i_op->getattr(ip, (struct kstat *)kstat_buf);
  inode_put(ip);
  if (rc != 0)
    return rc;
  if (copy_to_user(stat_buf, kstat_buf, sizeof(struct kstat)))
    return (int64_t)-EFAULT;
  return 0;
}

// S07: resolve a *at dirfd to its starting directory inode (+1, caller puts),
// or ERR_PTR(-errno). AT_FDCWD resolves to the root mount's root inode — this
// kernel has no per-process CWD (no chdir), so AT_FDCWD ≡ "from root", matching
// the existing absolute-path-only behaviour. A real dirfd must reference an
// open directory (ENOTDIR otherwise). Absolute paths are handled by the caller
// (fall back to sys_open/sys_stat/etc) before calling this.
struct inode *resolve_dirfd_start(int dirfd) {
  if (dirfd == AT_FDCWD) {
    struct mount_entry *m = mount_of_inode(NULL); /* root mount "/" */
    if (!m || !m->fs->mount_root)
      return ERR_PTR(-ENOENT);
    return m->fs->mount_root(m); /* +1 */
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
    ip = inode_get(f->inode); /* +1 */
  file_put(f);
  return ip;
}

// openat(dirfd, path, flags, mode). Absolute path → sys_open (mount-table
// match, unchanged). Relative path → resolve from dirfd's directory inode via
// path_walk_from/path_walk_parent_from. AT_FDCWD → from root (no CWD exists).
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

  /* Absolute path: dirfd ignored, resolve via mount table (existing path). */
  if (kpath[0] == '/')
    return sys_open(path, flags, mode, 0, 0, 0);

  /* Relative path: resolve start inode from dirfd (or root for AT_FDCWD). */
  struct inode *start = resolve_dirfd_start((int)dirfd);
  if (IS_ERR(start))
    return (int64_t)PTR_ERR(start);

  char relpath[256];
  if (normalize_path(kpath, relpath, sizeof(relpath)) < 0) {
    inode_put(start);
    return (int64_t)-ENAMETOOLONG;
  }

  int iflags = (int)flags;
  struct inode *ip = path_walk_from(start, relpath); /* +1 or NULL */
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
    ip = parent->i_op->create(parent, lastname, eff_mode); /* +1 */
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
  /* ip is +1 (from path_walk_from or create). */
  ip->mount = mount_of_inode(ip); /* lazy, mirrors sys_open */

  if (ip->type == INODE_DIR &&
      (iflags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC))) {
    inode_put(ip);
    return (int64_t)-EISDIR;
  }
  /* O_DIRECTORY: caller requires a directory; non-dir → ENOTDIR (Linux). */
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
  if (ip->type == INODE_REGULAR && ip->mount &&
      __strcmp(ip->mount->fs->name, "sysfs") == 0)
    f->f_op = &sysfs_fops;
  if (ip->type == INODE_REGULAR && ip->mount &&
      __strcmp(ip->mount->fs->name, "tmpfs") == 0)
    f->f_op = &tmpfs_file_fops;
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

// newfstatat(dirfd, path, buf, flags). Absolute path → sys_stat. AT_EMPTY_PATH
// with empty path → stat the dirfd itself (delegates to sys_fstat). Relative
// path → resolve from dirfd. AT_SYMLINK_NOFOLLOW accepted but a no-op (no
// symlinks in this OS).
int64_t sys_newfstatat(int64_t dirfd, int64_t path, int64_t buf, int64_t flags,
                       int64_t unused1, int64_t unused2) {
  (void)unused1;
  (void)unused2;
  if ((int)flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH))
    return -EINVAL;

  const char __user *upath = (const char __user *__force)path;
  char kpath[256];
  long n = strncpy_from_user(kpath, upath, sizeof(kpath));
  if (n < 0)
    return (int64_t)-EFAULT;

  /* AT_EMPTY_PATH: operate on dirfd itself (path == ""). */
  if (((int)flags & AT_EMPTY_PATH) && kpath[0] == '\0') {
    if ((int)dirfd == AT_FDCWD || dirfd < 0)
      return -EINVAL;
    return sys_fstat(dirfd, buf, 0, 0, 0, 0);
  }

  /* Absolute path: dirfd ignored, resolve via mount table (existing path). */
  if (kpath[0] == '/')
    return sys_stat(path, buf, 0, 0, 0, 0);

  /* Relative path: resolve start inode from dirfd (or root for AT_FDCWD). */
  struct inode *start = resolve_dirfd_start((int)dirfd);
  if (IS_ERR(start))
    return (int64_t)PTR_ERR(start);

  char relpath[256];
  if (normalize_path(kpath, relpath, sizeof(relpath)) < 0) {
    inode_put(start);
    return (int64_t)-ENAMETOOLONG;
  }

  struct inode *ip = path_walk_from(start, relpath); /* +1 */
  inode_put(start);
  if (!ip)
    return (int64_t)-ENOENT;
  uint8_t kstat_buf[256];
  int rc = -ENOSYS;
  if (ip->i_op && ip->i_op->getattr)
    rc = ip->i_op->getattr(ip, (struct kstat *)kstat_buf);
  inode_put(ip);
  if (rc != 0)
    return (int64_t)rc;
  if (copy_to_user((void __user *__force)buf, kstat_buf, sizeof(struct kstat)))
    return (int64_t)-EFAULT;
  return 0;
}

/* sys_truncate(path, len) — SYS_TRUNCATE (group 3)
 * Resolve the path to an inode via mount framework + path_walk, then
 * dispatch size change to i_op->setattr (eliminates raw fat32_open). */
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
  struct inode *ip = path_walk(m, relpath); /* +1 */
  if (!ip)
    return (int64_t)-ENOENT;
  if (ip->type != INODE_REGULAR) {
    inode_put(ip);
    return (int64_t)-EISDIR;
  }
  if (!ip->i_op || !ip->i_op->setattr) {
    inode_put(ip);
    return (int64_t)-EPERM; /* 对齐 Linux notify_change:无 setattr → EPERM */
  }
  int rc = ip->i_op->setattr(ip, (uint64_t)len); /* 锁由 setattr 内部持(§6.6) */
  inode_put(ip);
  return (int64_t)rc;
}

/* sys_fsync(fd) — SYS_FSYNC (group 3): write back dirty pages of one inode. */
int64_t sys_fsync(int64_t arg1, int64_t unused1, int64_t unused2,
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
  struct inode *ip = f->inode;
  rcu_read_unlock();
  if (!ip)
    return (int64_t)-EBADF;

  page_cache_flush_inode(ip);
  /* FAT32 has no separate metadata journal; the dir entry is updated
   * synchronously on each size/metadata change, so nothing more to flush. */
  return 0;
}

/* sys_sync() — SYS_SYNC (group 3): write back all dirty pages. */
int64_t sys_sync(int64_t unused1, int64_t unused2, int64_t unused3,
                 int64_t unused4, int64_t unused5, int64_t unused6) {
  (void)unused1;
  (void)unused2;
  (void)unused3;
  (void)unused4;
  (void)unused5;
  (void)unused6;
  page_cache_flush_all();
  return 0;
}

/* sys_mkdir(path, mode) — SYS_MKDIR */
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
    return (int64_t)-EPERM; /* 对齐 Linux vfs_mkdir:无 mkdir → EPERM */
  }
  int eff_mode = ((int)arg2 & 0777) & ~(int)current_proc->umask;
  rc = parent->i_op->mkdir(parent, lastname, eff_mode);
  inode_put(parent);
  if (rc != 0)
    return (int64_t)rc;
  /* S08: mkdir 不返 inode,取回新建目录设 owner=创建进程 + 应用 umask 权限位
   * (保留目录类型位 S_IFDIR)。 */
  struct inode *nip = path_walk(m, relpath); /* +1 */
  if (nip) {
    nip->mode = (nip->mode & ~0777) | (uint32_t)eff_mode;
    nip->uid = current_proc->uid;
    nip->gid = current_proc->gid;
    inode_put(nip);
  }
  return 0;
}

/* sys_mknod(path, mode, dev) — SYS_MKNOD
 * 对齐 Linux mknod：在 path 父目录建 mode 类型节点。
 * tmpfs 支持 S_IFREG/S_IFIFO/S_IFSOCK（建节点返 0）；
 * S_IFCHR/S_IFBLK/S_IFDIR（设备节点归 devtmpfs，目录用 mkdir）→ -EOPNOTSUPP。
 */
int64_t sys_mknod(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                  int64_t unused2, int64_t unused3) {
  (void)unused1;
  (void)unused2;
  (void)unused3;
  const char __user *upath = (const char __user *__force)arg1;
  int mode = (int)arg2;
  (void)arg3; /* dev 参数仅对 CHR/BLK 有意义，本方案支持类型不涉及，忽略 */

  if (!upath)
    return (int64_t)-EFAULT;
  int fmt = mode & S_IFMT;
  if (fmt != S_IFREG && fmt != S_IFIFO && fmt != S_IFSOCK)
    return (int64_t)-EOPNOTSUPP; /* 对齐 Linux：tmpfs 不建设备/目录节点 */

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
  /* S08: 权限位应用 umask,类型位保留;设 owner=创建进程。 */
  int eff_mode = (mode & S_IFMT) | ((mode & 0777) & ~(int)current_proc->umask);
  struct inode *ip = parent->i_op->create(parent, lastname, eff_mode);
  inode_put(parent);
  if (IS_ERR(ip))
    return PTR_ERR(ip);
  if (!ip)
    return (int64_t)-ENOMEM;
  ip->uid = current_proc->uid;
  ip->gid = current_proc->gid;
  inode_put(ip); /* create 已 inode_create 初始 +1，平衡 */
  return 0;
}

/* sys_unlink(path) — SYS_UNLINK */
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
    return (int64_t)-EPERM; /* 对齐 Linux vfs_unlink:无 unlink → EPERM */
  }
  rc = parent->i_op->unlink(parent, lastname);
  inode_put(parent);
  return (int64_t)rc;
}

/* sys_rename(oldpath, newpath) — SYS_RENAME
 * 照 sys_unlink 模板:双 path_walk_parent 取两个 parent + lastname,
 * 调 old_parent->i_op->rename。跨 mount 不支持(vfs_resolve 已剥离挂载点
 * 前缀,relpath 限单 mount 内),db 场景全在 /run/udev/data/ 单 tmpfs mount。 */
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

  /* db 场景 old/new 同 mount;跨 mount 返 -EXDEV(对齐 Linux rename(2)) */
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
    return (int64_t)-EPERM; /* 对齐 Linux vfs_rename:无 rename → EPERM */
  }
  rc = old_parent->i_op->rename(old_parent, old_name, new_parent, new_name);
  inode_put(old_parent);
  inode_put(new_parent);
  return (int64_t)rc;
}

/* sys_rmdir(path) — SYS_RMDIR */
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
    return (int64_t)-EPERM; /* 对齐 Linux vfs_rmdir:无 rmdir → EPERM */
  }
  rc = parent->i_op->rmdir(parent, lastname);
  inode_put(parent);
  return (int64_t)rc;
}

/* sys_dev_create(name, shm_fd, minor) — SYS_DEV_CREATE
 * Kernel auto-fills driver_pid=current_task->pid, is_block=false, callbacks
 * NULL (user-space driver). minor stored in dev_ops for ioctl req routing. */
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

/* sys_getdents(fd, buf, len) — SYS_GETDENTS64
 * Read directory entries into user buffer.
 * fd must be FD_DIR. Returns bytes written, 0 on EOF, or negative errno. */
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

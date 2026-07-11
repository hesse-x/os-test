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
      devtmpfs_create("sda", &blk_dev_ops, NULL);
      break;
    }
  }
  if (rc != 0) {
    printk(LOG_ERROR, "vfs_init: FAT32 init failed on all ports\n");
    return;
  }
}

/* sys_open(path, flags, mode) — SYS_OPEN */
int64_t sys_open(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                 int64_t unused2, int64_t unused3) {
  const char __user *upath = (const char __user *__force)arg1;
  int flags = (int)arg2;
  /* mode (arg3) ignored for FAT32 */

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

  /* 3. fstype lookup */
  int errno_val = 0;
  struct inode *ip;
  if (m->fs->lookup) {
    ip = m->fs->lookup(relpath);
    if (ip) {
      /* O_EXCL: file must not already exist. The lookup-based path
       * bypasses fat32_open's flag handling, so check here. */
      if ((flags & O_CREAT) && (flags & O_EXCL)) {
        inode_put(ip);
        return (int64_t)-EEXIST;
      }
      /* O_TRUNC: truncate existing regular file to 0. fat32_open did
       * this internally; the lookup path bypasses it. */
      if ((flags & O_TRUNC) && ip->type == INODE_REGULAR && ip->size > 0) {
        spin_lock(&ip->i_lock);
        fat32_ftruncate(ip, 0);
        spin_unlock(&ip->i_lock);
      }
    }
    if (!ip) {
      /* O_CREAT: fall back to fat32_open (creates the file) */
      if (flags & O_CREAT) {
        char full[258];
        full[0] = '/';
        size_t i = 0;
        while (relpath[i] && i < 256) {
          full[i + 1] = relpath[i];
          i++;
        }
        full[i + 1] = '\0';
        ip = fat32_open(full, flags, &errno_val);
        if (!ip)
          return errno_val;
      } else {
        return (int64_t)-ENOENT;
      }
    }
  } else {
    /* fallback: fat32_open with O_CREAT semantics */
    char full[258];
    full[0] = '/';
    size_t i = 0;
    while (relpath[i] && i < 256) {
      full[i + 1] = relpath[i];
      i++;
    }
    full[i + 1] = '\0';
    ip = fat32_open(full, flags, &errno_val);
    if (!ip)
      return errno_val;
  }
  ip->mount = m;

  /* Reject write access to directories (POSIX EISDIR). This also guards
   * against a directory inode leaking into a writable fd: previously a
   * FAT32 empty file collided with a devtmpfs dir inode at ino=0 and
   * open(...,O_WRONLY|O_CREAT) silently returned a dir fd, which later
   * crashed sys_read/sys_write. Returning EISDIR turns that into a
   * userspace error instead of a kernel page fault. */
  if (ip->type == INODE_DIR &&
      (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC))) {
    inode_put(ip);
    return (int64_t)-EISDIR;
  }

  /* 4. Allocate fd (under fd_lock) */
  xtask *proc = current_task;
  files *files = proc->proc->files;
  spinlock *fdlk = &files->fd_lock;
  spin_lock(fdlk);
  int fd = alloc_fd(files, 3);
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

  uint8_t kstat_buf[256];
  int rc;
  if (m->fs->stat)
    rc = m->fs->stat(relpath, (struct kstat *)kstat_buf);
  else
    return (int64_t)-ENOSYS;
  if (rc != 0)
    return rc;
  if (copy_to_user(stat_buf, kstat_buf, sizeof(struct kstat)))
    return (int64_t)-EFAULT;
  return 0;
}

/* sys_truncate(path, len) — SYS_TRUNCATE (group 3)
 * Resolve the path to an inode and grow/shrink the file to len bytes. */
int64_t sys_truncate(int64_t arg1, int64_t arg2, int64_t unused1,
                     int64_t unused2, int64_t unused3, int64_t unused4) {
  const char __user *upath = (const char __user *__force)arg1;
  int64_t len = arg2;
  if (!upath)
    return (int64_t)-EFAULT;
  if (len < 0)
    return (int64_t)-EINVAL;
  char path[256];
  if (strncpy_from_user(path, upath, 256) < 0)
    return (int64_t)-EFAULT;

  int err = 0;
  struct inode *ip = fat32_open(path, 0, &err);
  if (!ip)
    return (int64_t)(-err);
  spin_lock(&ip->i_lock);
  int rc = fat32_ftruncate(ip, (uint64_t)len);
  spin_unlock(&ip->i_lock);
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
  char relpath[256];
  struct mount_entry *m = vfs_resolve_user(upath, relpath, sizeof(relpath));
  if (IS_ERR(m))
    return PTR_ERR(m);
  if (!m || !m->fs->mkdir)
    return (int64_t)-ENOSYS;

  int rc = m->fs->mkdir(relpath);
  if (rc != 0)
    return rc;
  return 0;
}

/* sys_unlink(path) — SYS_UNLINK */
int64_t sys_unlink(int64_t arg1, int64_t unused1, int64_t unused2,
                   int64_t unused3, int64_t unused4, int64_t unused5) {
  const char __user *upath = (const char __user *__force)arg1;

  if (!upath)
    return (int64_t)-EFAULT;
  char relpath[256];
  struct mount_entry *m = vfs_resolve_user(upath, relpath, sizeof(relpath));
  if (IS_ERR(m))
    return PTR_ERR(m);
  if (!m || !m->fs->unlink)
    return (int64_t)-ENOSYS;

  int rc = m->fs->unlink(relpath);
  if (rc != 0)
    return rc;
  return 0;
}

/* sys_rmdir(path) — SYS_RMDIR */
int64_t sys_rmdir(int64_t arg1, int64_t unused1, int64_t unused2,
                  int64_t unused3, int64_t unused4, int64_t unused5) {
  const char __user *upath = (const char __user *__force)arg1;

  if (!upath)
    return (int64_t)-EFAULT;
  char relpath[256];
  struct mount_entry *m = vfs_resolve_user(upath, relpath, sizeof(relpath));
  if (IS_ERR(m))
    return PTR_ERR(m);
  if (!m || !m->fs->rmdir)
    return (int64_t)-ENOSYS;

  int rc = m->fs->rmdir(relpath);
  if (rc != 0)
    return rc;
  return 0;
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

/* sys_getdents(fd, buf, len) — SYS_GETDENTS
 * Read directory entries into user buffer.
 * fd must be FD_DIR. Returns bytes written, 0 on EOF, or negative errno. */
int64_t sys_getdents(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                     int64_t unused2, int64_t unused3) {
  int fd = (int)arg1;
  void __user *buf = (void __user *__force)arg2;
  size_t len = (size_t)arg3;

  if (fd < 0 || fd >= MAX_FD)
    return (int64_t)-EINVAL;
  if (len == 0 || len > 4096)
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

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/bsd/devtmpfs.h"
#include "arch/x64/utils.h"
#include "kernel/bsd/inode.h"
#include "kernel/bsd/mount.h"
#include "kernel/bsd/netlink.h"
#include "kernel/bsd/proc.h"
#include "kernel/bsd/types.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/kasan.h" // copy_from_user/strncpy_from_user/__user
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/trap.h"
#include "kernel/xcore/xtask.h"
#include "xos/fcntl.h"
#include <stddef.h>
#include <xos/errno.h>
#include <xos/stat.h>

#include "kernel/bsd/syscall.h"
#include "kernel/bsd/sysfs.h"

struct shm;

#define MAX_DEV_ENTRIES 32

struct dev_entry {
  char name[32];
  struct inode *ip;
  struct dev_entry *next;
};

/* Directory entry for subdirectory support (e.g. "dri") */
struct dev_dir {
  char name[32];
  struct inode *ip; /* INODE_DIR inode */
  struct dev_dir *next;
};

static struct dev_dir dev_dirs[16]; /* max 16 subdirectories */
static struct dev_dir *dir_list = NULL;
static int dir_count = 0;

static struct dev_entry dev_entries[MAX_DEV_ENTRIES];
static struct dev_entry *dev_list = NULL;
static int dev_count = 0;
static spinlock devtmpfs_lock = SPINLOCK_INIT;

static bool devtmpfs_initialized = false;
/* Dedicated root /dev inode — allows getdents to distinguish root from
 * subdirectory */
static struct inode *devtmpfs_root_ip = NULL;

/* Forward: devtmpfs_iget defined later (after devtmpfs_get_or_create_dir),
 * but devtmpfs_init / devtmpfs_get_or_create_dir call it. */
static struct inode *devtmpfs_iget(int type);

void devtmpfs_init(void) {
  if (devtmpfs_initialized) {
    printk(LOG_INFO, "devtmpfs_init: already initialized, skip\n");
    return;
  }
  spin_lock(&devtmpfs_lock);
  dev_list = NULL;
  dev_count = 0;
  dir_list = NULL;
  dir_count = 0;
  for (int i = 0; i < MAX_DEV_ENTRIES; i++) {
    dev_entries[i].name[0] = '\0';
    dev_entries[i].ip = NULL;
    dev_entries[i].next = NULL;
  }
  for (int i = 0; i < 16; i++) {
    dev_dirs[i].name[0] = '\0';
    dev_dirs[i].ip = NULL;
    dev_dirs[i].next = NULL;
  }
  spin_unlock(&devtmpfs_lock);

  /* Create dedicated root inode for /dev (distinguished from subdirectories).
   * Must be outside the lock because inode_create may allocate. */
  devtmpfs_root_ip = devtmpfs_iget(INODE_DIR);
  devtmpfs_initialized = true;
  printk(LOG_INFO, "devtmpfs_init: done\n");
}

/* Find or create a subdirectory dev_dir entry by name (no slash in name) */
static struct dev_dir *devtmpfs_find_dir(const char *name) {
  for (struct dev_dir *d = dir_list; d; d = d->next) {
    if (__strcmp(name, d->name) == 0)
      return d;
  }
  return NULL;
}

static struct dev_dir *devtmpfs_get_or_create_dir(const char *name, int len) {
  char tmp[32];
  if (len >= 31)
    return NULL;
  for (int i = 0; i < len; i++)
    tmp[i] = name[i];
  tmp[len] = '\0';
  struct dev_dir *d = devtmpfs_find_dir(tmp);
  if (d)
    return d;
  if (dir_count >= 16)
    return NULL;
  /* find free slot */
  for (int i = 0; i < 16; i++) {
    if (dev_dirs[i].ip == NULL) {
      struct inode *ip = devtmpfs_iget(INODE_DIR);
      if (!ip)
        return NULL;
      for (int j = 0; j < len; j++)
        dev_dirs[i].name[j] = tmp[j];
      dev_dirs[i].name[len] = '\0';
      dev_dirs[i].ip = ip;
      dev_dirs[i].ip->i_priv = &dev_dirs[i]; /* I5:子目录 inode 回指 dev_dir,供 lookup 取 prefix */
      dev_dirs[i].next = dir_list;
      dir_list = &dev_dirs[i];
      dir_count++;
      return &dev_dirs[i];
    }
  }
  return NULL;
}

/* devtmpfs_iget:封装 inode_create + 挂 i_op。devtmpfs inode 由
 *  dev_entries[].ip/dev_dirs[].ip 持基准 ref 常驻。 */
static const struct inode_operations devtmpfs_dir_iop;
static const struct inode_operations devtmpfs_dev_iop;

static struct inode *devtmpfs_iget(int type) {
  struct inode *ip = inode_create(0, type, 0, 0, 0, 0);
  if (!ip)
    return NULL;
  ip->i_op = (type == INODE_DIR) ? &devtmpfs_dir_iop : &devtmpfs_dev_iop;
  return ip;
}

/* devtmpfs_dir_lookup:在目录 inode dir 内查名为 name 的直接子项,返 +1 inode 或 NULL。
 *  I4(a) 决议:dev_list 存全名(如 "dri/card0"),path_walk 逐段收单名,故按 dir 身份取
 *  prefix 拼全名比较。根(dir->i_priv==NULL)扁平匹配 top-level;子目录(i_priv==dev_dir*)
 *  拼 "prefix/name" 匹配。守 +1 inode_get 契约(修复旧 devtmpfs_lookup 借用无 get,UAF)。 */
static struct inode *devtmpfs_dir_lookup(struct inode *dir, const char *name) {
  if (name[0] == '\0') {
    if (devtmpfs_root_ip) {
      inode_get(devtmpfs_root_ip);
      return devtmpfs_root_ip;
    }
    return NULL;
  }
  int namelen = 0;
  while (name[namelen])
    namelen++;
  struct dev_dir *dd = (struct dev_dir *)dir->i_priv;
  const char *prefix = dd ? dd->name : NULL;
  int prefix_len = 0;
  if (prefix) {
    while (prefix[prefix_len])
      prefix_len++;
  }
  spin_lock(&devtmpfs_lock);
  struct dev_entry *e = dev_list;
  while (e) {
    int elen = 0;
    while (e->name[elen])
      elen++;
    if (!prefix) {
      /* 根:仅匹配 top-level(无 '/' 的全名) */
      int has_slash = 0;
      for (int i = 0; i < elen; i++)
        if (e->name[i] == '/') { has_slash = 1; break; }
      if (!has_slash && elen == namelen &&
          __memcmp(e->name, name, namelen) == 0) {
        inode_get(e->ip);
        spin_unlock(&devtmpfs_lock);
        return e->ip;
      }
    } else {
      /* 子目录:匹配 "prefix/name" */
      if (elen == prefix_len + 1 + namelen &&
          e->name[prefix_len] == '/' &&
          __memcmp(e->name, prefix, prefix_len) == 0 &&
          __memcmp(e->name + prefix_len + 1, name, namelen) == 0) {
        inode_get(e->ip);
        spin_unlock(&devtmpfs_lock);
        return e->ip;
      }
    }
    e = e->next;
  }
  /* 子目录内不再嵌套 dev_dir(现状只支持一级),不查 dir_list */
  if (!prefix) {
    struct dev_dir *d = dir_list;
    while (d) {
      if (__strcmp(name, d->name) == 0) {
        inode_get(d->ip);
        spin_unlock(&devtmpfs_lock);
        return d->ip;
      }
      d = d->next;
    }
  }
  spin_unlock(&devtmpfs_lock);
  return NULL;
}

/* devtmpfs_getattr:从 ip 字段填。根 st_ino=ip->ino(修正旧硬编码 0)。
 *  st_rdev=ip->ino 是设备号架构 gap,记 todo 不动(§3.5)。 */
static int devtmpfs_getattr(struct inode *ip, struct kstat *ks) {
  __memset(ks, 0, sizeof(*ks));
  if (ip->type == INODE_DIR) {
    ks->st_mode = 0040755;
  } else {
    ks->st_mode = 0020000 | 0600; /* S_IFCHR | 0600 */
    ks->st_rdev = (uint64_t)ip->ino; /* 设备号=inode(现状) */
  }
  ks->st_ino = ip->ino;
  ks->st_nlink = 1;
  ks->st_size = 0;
  ks->st_blksize = 512;
  return 0;
}

static const struct inode_operations devtmpfs_dir_iop = {
    .lookup = devtmpfs_dir_lookup,
    .getattr = devtmpfs_getattr,
};

static const struct inode_operations devtmpfs_dev_iop = {
    .getattr = devtmpfs_getattr,
};

/* devtmpfs_mount_root:返回 /dev 根 inode(已 inode_get)。 */
static struct inode *devtmpfs_mount_root(struct mount_entry *m) {
  (void)m;
  if (!devtmpfs_root_ip)
    return NULL;
  return inode_get(devtmpfs_root_ip);
}

struct inode *devtmpfs_lookup(const char *name) {
  /* relpath from vfs_resolve has no /dev/ prefix; entries store paths
   * relative to /dev (e.g. "serial", "dri/card0"). */
  /* Empty string = root /dev directory — return the dedicated root inode. */
  if (name[0] == '\0') {
    if (devtmpfs_root_ip) {
      inode_get(devtmpfs_root_ip);
      return devtmpfs_root_ip;
    }
    return NULL;
  }

  /* If path contains '/', split into dir + leaf */
  const char *slash = name;
  while (*slash && *slash != '/')
    slash++;
  if (*slash == '/') {
    int dir_len = slash - name;
    char dir_name[32];
    if (dir_len >= 31)
      return NULL;
    for (int i = 0; i < dir_len; i++)
      dir_name[i] = name[i];
    dir_name[dir_len] = '\0';
    spin_lock(&devtmpfs_lock);
    struct dev_dir *d = devtmpfs_find_dir(dir_name);
    spin_unlock(&devtmpfs_lock);
    if (!d)
      return NULL;
    /* lookup leaf inside dir: match by full path (stored entry.name includes
     * dir/ prefix) */
    spin_lock(&devtmpfs_lock);
    struct dev_entry *e = dev_list;
    while (e) {
      if (__strcmp(name, e->name) == 0) {
        spin_unlock(&devtmpfs_lock);
        return e->ip;
      }
      e = e->next;
    }
    spin_unlock(&devtmpfs_lock);
    return NULL;
  }
  /* No slash: flat lookup — search devices first, then directories */
  spin_lock(&devtmpfs_lock);
  struct dev_entry *e = dev_list;
  while (e) {
    if (__strcmp(name, e->name) == 0) {
      spin_unlock(&devtmpfs_lock);
      return e->ip;
    }
    e = e->next;
  }
  /* Check directories */
  struct dev_dir *d = dir_list;
  while (d) {
    if (__strcmp(name, d->name) == 0) {
      spin_unlock(&devtmpfs_lock);
      return d->ip;
    }
    d = d->next;
  }
  spin_unlock(&devtmpfs_lock);
  return NULL;
}

int devtmpfs_create(const char *name, struct dev_ops *ops, struct shm *shm) {
  WARN_ON(!devtmpfs_initialized); // catch order bugs: create before init
  if (dev_count >= MAX_DEV_ENTRIES)
    return -ENOMEM;

  /* Check if already exists (full path) */
  if (devtmpfs_lookup(name))
    return -EEXIST;

  /* If path contains '/', create the subdirectory first */
  const char *slash = name;
  while (*slash && *slash != '/')
    slash++;
  if (*slash == '/') {
    int dir_len = slash - name;
    spin_lock(&devtmpfs_lock);
    devtmpfs_get_or_create_dir(name, dir_len);
    spin_unlock(&devtmpfs_lock);
  }

  spin_lock(&devtmpfs_lock);

  /* Find free slot */
  int slot = -1;
  for (int i = 0; i < MAX_DEV_ENTRIES; i++) {
    if (dev_entries[i].ip == NULL) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    spin_unlock(&devtmpfs_lock);
    return -ENOMEM;
  }

  /* Create inode */
  struct inode *ip = devtmpfs_iget(INODE_DEV);
  if (!ip) {
    spin_unlock(&devtmpfs_lock);
    return -ENOMEM;
  }
  ip->i_priv = ops;
  if (shm) {
    shm_get(shm); // +1 for inode reference
    ip->shm = shm;
  } else {
    ip->shm = NULL;
  }

  /* Fill entry — store full path including any '/' */
  int i;
  for (i = 0; name[i] && i < 31; i++)
    dev_entries[slot].name[i] = name[i];
  dev_entries[slot].name[i] = '\0';
  dev_entries[slot].ip = ip;
  dev_entries[slot].next = dev_list;
  dev_list = &dev_entries[slot];
  dev_count++;

  spin_unlock(&devtmpfs_lock);
  printk(LOG_INFO, "devtmpfs: created /dev/%s\n", name);

  // Broadcast uevent only for kernel devices (user-space drivers push via
  // SYS_DEV_SET_META after metadata is set — design 3.3.2 step 2)
  if (devtmpfs_initialized && nl_is_initialized() && ops &&
      ops->driver_pid == 0) {
    const char *subsys =
        ops->subsystem[0] ? ops->subsystem : (ops->is_block ? "block" : "misc");
    nl_uevent_broadcast("add", name, subsys);
  }
  return 0;
}

uint64_t devtmpfs_open(xtask *proc, const char *name, int flags,
                       struct mount_entry *m) {
  struct inode *ip = devtmpfs_lookup(name);
  if (!ip)
    return (uint64_t)(-(uint64_t)ENOENT);

  /* Handle directories: create FD_DIR (not FD_DEV) so getdents works.
   * Also set ip->mount so mount_of_inode() finds the devtmpfs fstype. */
  if (ip->type == INODE_DIR) {
    ip->mount = m;
    files *fs = proc->proc->files;
    spinlock *fdlk = &fs->fd_lock;
    spin_lock(fdlk);
    int fd = alloc_fd(fs, 3);
    if (fd < 0) {
      spin_unlock(fdlk);
      return (uint64_t)(-(uint64_t)EMFILE);
    }
    struct file *f = kmalloc(sizeof(struct file));
    if (!f) {
      spin_unlock(fdlk);
      return (uint64_t)(-(uint64_t)ENOMEM);
    }
    __memset(f, 0, sizeof(*f));
    refcount_set(&f->f_count, 1);
    f->type = FD_DIR;
    f->flags = O_RDONLY;
    f->inode = ip;
    inode_get(ip);
    f->offset = 0;
    fd_install(fs, fd, f);
    spin_unlock(fdlk);
    return (uint64_t)fd;
  }

  /* Device open path (existing) */
  spinlock *fdlk = &proc->proc->files->fd_lock;
  spin_lock(fdlk);
  int fd = alloc_fd(proc->proc->files, 3);
  if (fd < 0) {
    spin_unlock(fdlk);
    return (uint64_t)(-(uint64_t)EMFILE);
  }

  struct file *f = kmalloc(sizeof(struct file));
  if (!f) {
    spin_unlock(fdlk);
    return (uint64_t)(-(uint64_t)ENOMEM);
  }
  __memset(f, 0, sizeof(*f));
  refcount_set(&f->f_count, 1);
  f->type = FD_DEV;
  f->flags = flags;
  f->inode = ip;
  inode_get(ip);

  // Install FD_DEV BEFORE ops->open so callbacks (e.g. pts_open/ptmx_open)
  // can access it via fd_table[fd] and mutate it into FD_TTY in place.
  fd_install(proc->proc->files, fd, f);

  if (ip->i_priv) {
    struct dev_ops *ops = (struct dev_ops *)ip->i_priv;
    f->target_pid = ops->driver_pid;
    /* SHM-backed 用户态驱动 = ringbuf (design 2.5) */
    if (ops->driver_pid > 0 && ip->shm) {
      f->f_op = &ringbuf_fops;
      ringbuf_init_cursor(ip, f);
    }
    /* ringbuf lifecycle: send RINGBUF_OPEN to driver (design 3.6) */
    if (ops->driver_pid > 0 && ip->shm)
      ringbuf_notify_open(ip, proc->pid);
    // Kernel device: call open callback. Callbacks mutate the FD_DEV file
    // in place (do not replace the pointer), so fd_table[fd] stays valid.
    if (ops->driver_pid == 0 && ops->open) {
      int rc = ops->open(proc, fd);
      if (rc < 0) {
        // Open failed: undo fd installation.
        // Manual cleanup (not file_put) to avoid calling ops->close
        // when ops->open itself failed.
        fd_uninstall(proc->proc->files, fd);
        inode_put(ip);
        kfree(f);
        spin_unlock(fdlk);
        return (uint64_t)(-(uint64_t)(-rc));
      }
    }
  }
  spin_unlock(fdlk);
  return (uint64_t)fd;
}

void devtmpfs_cleanup_pid(pid_t pid) {
  spin_lock(&devtmpfs_lock);
  struct dev_entry **pp = &dev_list;
  while (*pp) {
    struct dev_entry *e = *pp;
    if (e->ip && e->ip->i_priv) {
      struct dev_ops *ops = (struct dev_ops *)e->ip->i_priv;
      if (ops->driver_pid == pid) {
        /* Remove from list */
        *pp = e->next;
        /* Clean up sysfs subtree + subsys_priv */
        if (ops->sysfs_dir) {
          sysfs_remove_dir(ops->sysfs_dir);
          ops->sysfs_dir = NULL;
        }
        if (ops->subsys_priv) {
          kfree(ops->subsys_priv);
          ops->subsys_priv = NULL;
        }
        /* Free kmalloc'd dev_ops (user-space driver) */
        if (ops->driver_pid > 0)
          kfree(ops);
        /* Free inode */
        inode_put(e->ip);
        e->ip = NULL;
        e->name[0] = '\0';
        dev_count--;
        continue;
      }
    }
    pp = &e->next;
  }
  spin_unlock(&devtmpfs_lock);
}

void devtmpfs_remove(const char *name) {
  bool removed = false;
  spin_lock(&devtmpfs_lock);
  struct dev_entry **pp = &dev_list;
  while (*pp) {
    struct dev_entry *e = *pp;
    if (__strcmp(name, e->name) == 0) {
      *pp = e->next;
      if (e->ip)
        inode_put(e->ip);
      e->ip = NULL;
      e->name[0] = '\0';
      dev_count--;
      removed = true;
      break;
    }
    pp = &e->next;
  }
  spin_unlock(&devtmpfs_lock);

  if (removed && devtmpfs_initialized && nl_is_initialized())
    nl_uevent_broadcast("remove", name, "misc");
}

/* ==================== devtmpfs fstype callbacks ==================== */

/* getdents: enumerate direct children of a devtmpfs directory.
 * relpath "" or NULL = root /dev; "dri" = /dev/dri subdir.
 * Scans dev_list + dir_list, matching entries whose name starts with
 * dir + "/" and have no further "/" (direct children only). */
static ssize_t devtmpfs_getdents(struct inode *dir, struct dir_context *ctx) {
  spin_lock(&devtmpfs_lock);

  if (ctx->pos != 0) {
    spin_unlock(&devtmpfs_lock);
    return 0;
  }

  /* Determine if this is the root /dev directory or a subdirectory.
   * Root has a dedicated inode (devtmpfs_root_ip). */
  bool is_root = (devtmpfs_root_ip && dir->ino == devtmpfs_root_ip->ino);
  const char *prefix = NULL;
  int prefix_len = 0;

  if (!is_root) {
    /* Find which subdirectory this inode belongs to */
    struct dev_dir *dd = dir_list;
    while (dd) {
      if (dd->ip && dd->ip->ino == dir->ino) {
        prefix = dd->name;
        for (prefix_len = 0; prefix[prefix_len]; prefix_len++)
          ;
        break;
      }
      dd = dd->next;
    }
    /* If no matching dev_dir found, treat as root */
    is_root = (prefix == NULL);
  }

  if (is_root) {
    /* Root /dev: emit all directories and top-level devices (no '/' in name) */
    struct dev_dir *d = dir_list;
    while (d) {
      size_t nl = 0;
      while (d->name[nl])
        nl++;
      if (!dir_emit(ctx, d->name, (int)nl, ctx->written, d->ip->ino, DT_DIR))
        break;
      d = d->next;
    }

    struct dev_entry *e = dev_list;
    while (e) {
      int has_slash = 0;
      size_t nl = 0;
      while (e->name[nl]) {
        if (e->name[nl] == '/')
          has_slash = 1;
        nl++;
      }
      if (!has_slash) {
        if (!dir_emit(ctx, e->name, (int)nl, ctx->written, e->ip->ino, DT_CHR))
          break;
      }
      e = e->next;
    }
  } else {
    /* Subdirectory listing: emit only device entries whose name starts with
     * "prefix/" and has no further "/" after the prefix. */
    struct dev_entry *e = dev_list;
    while (e) {
      size_t nl = 0;
      while (e->name[nl])
        nl++;
      /* Check if name starts with "prefix/" */
      if ((int)nl > prefix_len + 1 && e->name[prefix_len] == '/' &&
          __strncmp(e->name, prefix, (size_t)prefix_len) == 0) {
        const char *leaf = e->name + prefix_len + 1;
        int has_inner_slash = 0;
        for (const char *p = leaf; *p; p++) {
          if (*p == '/') {
            has_inner_slash = 1;
            break;
          }
        }
        if (!has_inner_slash) {
          if (!dir_emit(ctx, leaf, (int)(nl - prefix_len - 1), ctx->written,
                        e->ip->ino, DT_CHR))
            break;
        }
      }
      e = e->next;
    }
  }

  ctx->pos = (uint64_t)-1; /* EOF */
  spin_unlock(&devtmpfs_lock);
  return (ssize_t)ctx->written;
}

/* R1 stub:返 NULL。R3(plan_vfs1.md)以 devtmpfs_mount_root 取代。 */
struct fstype devtmpfs_fstype = {
    .name = "devtmpfs",
    .mount_root = devtmpfs_mount_root,
    .getdents = devtmpfs_getdents,
};

/* sys_dev_set_meta(name, subsystem, devtype, props) — SYS_DEV_SET_META
 * Sets device metadata + builds sysfs subtree + pushes uevent.
 * Step 2 of two-step registration (design 3.3.2). */
int64_t sys_dev_set_meta(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4,
                         int64_t unused1, int64_t unused2) {
  (void)unused1;
  (void)unused2;
  const char __user *uname = (const char __user *__force)arg1;
  const char __user *usubsys = (const char __user *__force)arg2;
  const char __user *udevtype = (const char __user *__force)arg3;
  const char __user *uprops = (const char __user *__force)arg4;

  if (!uname || !usubsys || !udevtype)
    return (int64_t)-EFAULT;

  char name[32], subsystem[8], devtype[8];
  if (strncpy_from_user(name, uname, 32) < 0)
    return (int64_t)-EFAULT;
  if (strncpy_from_user(subsystem, usubsys, 8) < 0)
    return (int64_t)-EFAULT;
  if (strncpy_from_user(devtype, udevtype, 8) < 0)
    return (int64_t)-EFAULT;

  /* Find dev_ops by name. devtmpfs_lookup returns a borrowed reference
   * (no refcount increment) for non-root entries, so do NOT inode_put. */
  struct inode *ip = devtmpfs_lookup(name);
  if (!ip)
    return (int64_t)-ENOENT;
  struct dev_ops *ops = (struct dev_ops *)ip->i_priv;
  if (!ops) {
    return (int64_t)-ENOENT;
  }

  /* Fill subsystem/devtype */
  __strncpy(ops->subsystem, subsystem, 7);
  ops->subsystem[7] = '\0';
  __strncpy(ops->devtype, devtype, 7);
  ops->devtype[7] = '\0';

  /* Copy props if provided */
  struct input_dev_props *iprops = NULL;
  if (uprops) {
    iprops = kmalloc(sizeof(struct input_dev_props));
    if (!iprops) {
      return (int64_t)-ENOMEM;
    }
    if (copy_from_user(iprops, uprops, sizeof(struct input_dev_props))) {
      kfree(iprops);
      return (int64_t)-EFAULT;
    }
    ops->subsys_priv = iprops;
  }

  /* Build sysfs subtree for input devices with props */
  if (__strcmp(ops->subsystem, "input") == 0 && iprops) {
    const char *slash = name;
    while (*slash && *slash != '/')
      slash++;
    const char *basename = (*slash == '/') ? slash + 1 : name;

    struct sysfs_node *cls = sysfs_class_dir("input");
    struct sysfs_node *devdir = sysfs_create_dir(cls, basename);
    if (devdir) {
      /* Per-device attr copies: const templates have no priv; we need
       * priv = iprops so show callbacks read this device's properties.
       * (Shared mutable attrs would corrupt across multiple devices.) */
      const struct sysfs_attr *tmpl[5] = {
          &evdev_attr_name, &evdev_attr_bustype, &evdev_attr_vendor,
          &evdev_attr_product, &evdev_attr_version};
      const char *fnames[5] = {"name", "bustype", "vendor", "product",
                               "version"};
      struct sysfs_node *iddir = sysfs_create_dir(devdir, "id");
      for (int i = 0; i < 5; i++) {
        struct sysfs_attr *a = kmalloc(sizeof(*a));
        if (!a)
          break;
        a->name = tmpl[i]->name;
        a->priv = iprops;
        a->show = tmpl[i]->show;
        a->store = tmpl[i]->store;
        struct sysfs_node *target = (i == 0) ? devdir : iddir;
        struct sysfs_node *fn = sysfs_create_file(target, fnames[i], a);
        if (fn)
          fn->attr_owned = true;
        else
          kfree(a);
      }
      ops->sysfs_dir = devdir;
    }
  }

  /* Push uevent (step 2: device ready) */
  if (nl_is_initialized())
    nl_uevent_broadcast("add", name, ops->subsystem);

  return 0;
}

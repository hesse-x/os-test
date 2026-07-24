/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/bsd/devtmpfs.h"
#include "arch/x64/utils.h"
#include "kernel/bsd/evdev_broker.h"
#include "kernel/bsd/inode.h"
#include "kernel/bsd/mount.h"
#include "kernel/bsd/netlink.h"
#include "kernel/bsd/proc.h"
#include "kernel/bsd/types.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/kasan.h" // copy_from_user/strncpy_from_user/__user
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/trap.h"
#include "kernel/xcore/xtask.h"
#include "xos/fcntl.h"
#include <stddef.h>
#include <xos/dirent.h>
#include <xos/errno.h>
#include <xos/stat.h>

#include "kernel/bsd/syscall.h"
#include "kernel/bsd/sysfs.h"

/* DRM 主号（仅 stat 设备号用，与 virtio_gpu.c DRM_MAJOR 同值；devtmpfs 不
 * 反向 include 驱动头）。 */
#define DRM_MAJOR_FOR_STAT 226

struct shm;

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

static struct dev_dir *dir_list = NULL;

static struct dev_entry *dev_list = NULL;
static spinlock devtmpfs_lock = SPINLOCK_INIT;

static bool devtmpfs_initialized = false;
/* Dedicated root /dev inode — allows getdents to distinguish root from
 * subdirectory */
static struct inode *devtmpfs_root_ip = NULL;

/* §5: dev_ops 引用计数封装(FUSE fuse_conn 式)。
 *   get: 取引用(open 路径持 fd 引用);refcount_inc 内部 BUG_ON from-0 = UAF。
 *   put: 放引用(file_put 放 fd 引用、cleanup_pid 放注册引用);归 0 才 kfree。
 * 仅 user-space driver(driver_pid>0,kmalloc ops)会归 0;kernel device ops
 * 为 static,注册引用永在,refcount 永不归 0,put 不触发 kfree。fd 持引用
 * 覆盖 read/write/ioctl/poll 裸读 i_priv 的生命周期,故那些路径不必 get/put。*/
void dev_ops_get(struct dev_ops *ops) {
  ASSERT(ops);
  refcount_inc(&ops->refcount);
}

void dev_ops_put(struct dev_ops *ops) {
  if (!ops)
    return;
  if (refcount_dec_and_test(&ops->refcount)) {
    /* 仅 user-space driver 的 kmalloc ops 到此;kernel device ops 为 static
     * 永不归 0。subsys_priv/uevent_priv/sysfs_dir 已由 cleanup_pid 先行清理。*/
    kfree(ops);
  }
}

/* §5: 在 devtmpfs_lock 下读 inode->i_priv 返回 ops 指针(不加引用)。
 * 供 file_put(FD_DEV/FD_TTY) 等路径用:裸读 i_priv 与 cleanup_pid 的 put 之间
 * 存在 §3 同类 borrow-window(ops 在读后被 kfree),需在锁下读。调用方持有 fd
 * 引用(open 时 dev_ops_get 取),ops 在本 fd close 前不会归 0,故读出后无需再
 * get 即可安全用;close 回调 + 放 fd 引用(dev_ops_put)均在锁外完成。*/
struct dev_ops *dev_ops_peek_by_inode(struct inode *ip) {
  if (!ip)
    return NULL;
  spin_lock(&devtmpfs_lock);
  struct dev_ops *ops = ip->i_priv ? (struct dev_ops *)ip->i_priv : NULL;
  spin_unlock(&devtmpfs_lock);
  return ops;
}

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
  dir_list = NULL;
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
  struct inode *ip = devtmpfs_iget(INODE_DIR);
  if (!ip)
    return NULL;
  struct dev_dir *nd = kmalloc(sizeof(struct dev_dir));
  if (!nd) {
    inode_put(ip);
    return NULL;
  }
  for (int j = 0; j < len; j++)
    nd->name[j] = tmp[j];
  nd->name[len] = '\0';
  nd->ip = ip;
  nd->ip->i_priv = nd; /* I5:子目录 inode 回指 dev_dir,供 lookup 取 prefix */
  nd->next = dir_list;
  dir_list = nd;
  return nd;
}

/* devtmpfs_iget:封装 inode_create + 挂 i_op。devtmpfs inode 由
 *  dev_list/dev_dir 链表节点(kmalloc 动态分配)的 ip 持基准 ref 常驻。 */
static const struct inode_operations devtmpfs_dir_iop;
static const struct inode_operations devtmpfs_dev_iop;

static struct inode *devtmpfs_iget(int type) {
  struct inode *ip = inode_create(0, type, 0, 0, 0, 0);
  if (!ip)
    return NULL;
  ip->i_op = (type == INODE_DIR) ? &devtmpfs_dir_iop : &devtmpfs_dev_iop;
  return ip;
}

/* devtmpfs_dir_lookup:在目录 inode dir 内查名为 name 的直接子项,返 +1 inode 或
 * NULL。 I4(a) 决议:dev_list 存全名(如 "dri/card0"),path_walk 逐段收单名,故按
 * dir 身份取 prefix 拼全名比较。根(dir->i_priv==NULL)扁平匹配
 * top-level;子目录(i_priv==dev_dir*) 拼 "prefix/name" 匹配。守 +1 inode_get
 * 契约(修复旧 devtmpfs_lookup 借用无 get,UAF)。 */
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
        if (e->name[i] == '/') {
          has_slash = 1;
          break;
        }
      if (!has_slash && elen == namelen &&
          __memcmp(e->name, name, namelen) == 0) {
        inode_get(e->ip);
        spin_unlock(&devtmpfs_lock);
        return e->ip;
      }
    } else {
      /* 子目录:匹配 "prefix/name" */
      if (elen == prefix_len + 1 + namelen && e->name[prefix_len] == '/' &&
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
 *  st_rdev=ip->ino 是设备号架构 gap,记 todo 不动(§3.5)。
 *  S08: 报真实 ip->mode/uid/gid(devtmpfs inode 由内核建,owner 默认 0 root;
 *  mode 在 devtmpfs_create 时设 S_IFCHR|0600,dir 默认 0040755)。 */
static int devtmpfs_getattr(struct inode *ip, struct kstat *ks) {
  __memset(ks, 0, sizeof(*ks));
  if (ip->type == INODE_DIR) {
    ks->st_mode = ip->mode ? ip->mode : 0040755;
  } else {
    ks->st_mode = ip->mode ? ip->mode : (0020000 | 0600); /* S_IFCHR | 0600 */
    /* 设备号：DRM 设备返回真实 makedev(226, minor)（libdrm 靠 fstat.st_rdev
     * 判 render/primary，见 drmGetNodeTypeFromFd）；其余设备维持 =ino 现状
     *（架构 gap 记 todo §3.5，无消费者依赖）。子目录 inode 的 i_priv 是
     * dev_dir*，但本 else 分支只对字符设备 inode 进入，i_priv 必为 dev_ops*。
     */
    struct dev_ops *ops = (struct dev_ops *)ip->i_priv;
    if (ops && __strcmp(ops->subsystem, "drm") == 0)
      ks->st_rdev = k_makedev(DRM_MAJOR_FOR_STAT, ops->minor);
    else
      ks->st_rdev = (uint64_t)ip->ino;
  }
  ks->st_ino = ip->ino;
  ks->st_uid = ip->uid;
  ks->st_gid = ip->gid;
  ks->st_nlink = 1;
  ks->st_size = 0;
  ks->st_blksize = 4096;
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
     * dir/ prefix). Return a +1 reference taken under the lock so the inode
     * cannot be freed (by a concurrent devtmpfs_remove / cleanup_pid) between
     * the unlock here and the caller's first dereference of ip->i_priv — that
     * borrow window was bug.md §3 (UAF → ip->i_priv=0x10 → #PF). Matches the
     * devtmpfs_dir_lookup +1 contract; callers inode_put when done. */
    spin_lock(&devtmpfs_lock);
    struct dev_entry *e = dev_list;
    while (e) {
      if (__strcmp(name, e->name) == 0) {
        inode_get(e->ip);
        spin_unlock(&devtmpfs_lock);
        return e->ip;
      }
      e = e->next;
    }
    spin_unlock(&devtmpfs_lock);
    return NULL;
  }
  /* No slash: flat lookup — search devices first, then directories.
   * Same +1 contract as the slash path (see comment above). */
  spin_lock(&devtmpfs_lock);
  struct dev_entry *e = dev_list;
  while (e) {
    if (__strcmp(name, e->name) == 0) {
      inode_get(e->ip);
      spin_unlock(&devtmpfs_lock);
      return e->ip;
    }
    e = e->next;
  }
  /* Check directories */
  struct dev_dir *d = dir_list;
  while (d) {
    if (__strcmp(name, d->name) == 0) {
      inode_get(d->ip);
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

  /* Check if already exists (full path). devtmpfs_lookup now returns a +1
   * reference, so drop it before deciding. */
  struct inode *existing = devtmpfs_lookup(name);
  if (existing) {
    inode_put(existing);
    return -EEXIST;
  }

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
  struct dev_entry *ne = kmalloc(sizeof(struct dev_entry));
  if (!ne) {
    inode_put(ip);
    spin_unlock(&devtmpfs_lock);
    return -ENOMEM;
  }
  int i;
  for (i = 0; name[i] && i < 31; i++)
    ne->name[i] = name[i];
  ne->name[i] = '\0';
  ne->ip = ip;
  ne->next = dev_list;
  dev_list = ne;

  /* §5: 注册引用(dev_list entry 持有)。ops 进来时 refcount 已为 0(static/嵌入式
   * 由零初始化保证,kmalloc 由 __memset(0) 保证);此处建立首个引用代表"已注册"。
   * 不能用 dev_ops_get():refcount_inc 把 0→1 视作 UAF 并 BUG_ON;那是为"获取
   * 额外引用"路径(open/peek)设的 UAF 防护,不适用于此处 from-0 的
   * bootstrapping。 同一 ops 多次 create(如 random/urandom 共用 random_ops)→
   * 第二次起 refcount 已 ≥1,走 dev_ops_get 正常 +1;首次(==0)用 refcount_set
   * 建立。cleanup_pid 放 此引用,归 0(且无 fd 持引用)才 kfree。*/
  if (refcount_read(&ops->refcount) == 0)
    refcount_set(&ops->refcount, 1);
  else
    dev_ops_get(ops);

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
    int fd = alloc_fd(fs, 0);
    if (fd < 0) {
      spin_unlock(fdlk);
      inode_put(ip); /* drop the lookup reference on failure */
      return (uint64_t)(-(uint64_t)EMFILE);
    }
    struct file *f = kmalloc(sizeof(struct file));
    if (!f) {
      spin_unlock(fdlk);
      inode_put(ip); /* drop the lookup reference on failure */
      return (uint64_t)(-(uint64_t)ENOMEM);
    }
    __memset(f, 0, sizeof(*f));
    refcount_set(&f->f_count, 1);
    f->type = FD_DIR;
    f->flags = O_RDONLY;
    f->inode = ip; /* file takes ownership of the lookup +1 reference */
    f->offset = 0;
    fd_install(fs, fd, f);
    spin_unlock(fdlk);
    return (uint64_t)fd;
  }

  /* Device open path (existing) */
  /* §5: 在 fdlk 之前、独立 devtmpfs_lock 临界区取 ops 引用(fd 引用)。
   * fdlk 不防 devtmpfs_cleanup_pid(后者持 devtmpfs_lock),若在 fdlk 内裸读
   * i_priv 会与 cleanup_pid 的 put 形成 §3 同类 borrow-window(ops 在取引用前
   * 被 kfree)。先持 fd 引用,则本 fd 整个生命周期 ops 必活,read/write/ioctl/
   * poll 裸读 i_priv 安全。fd 引用在 file_put(FD_DEV) 放。*/
  spin_lock(&devtmpfs_lock);
  struct dev_ops *ops = ip->i_priv ? (struct dev_ops *)ip->i_priv : NULL;
  if (ops)
    dev_ops_get(ops);
  spin_unlock(&devtmpfs_lock);

  spinlock *fdlk = &proc->proc->files->fd_lock;
  spin_lock(fdlk);
  int fd = alloc_fd(proc->proc->files, 0);
  if (fd < 0) {
    spin_unlock(fdlk);
    if (ops)
      dev_ops_put(ops); /* §5: 放刚取的 fd 引用 */
    inode_put(ip);      /* drop the lookup reference on failure */
    return (uint64_t)(-(uint64_t)EMFILE);
  }

  struct file *f = kmalloc(sizeof(struct file));
  if (!f) {
    spin_unlock(fdlk);
    if (ops)
      dev_ops_put(ops); /* §5: 放刚取的 fd 引用 */
    inode_put(ip);      /* drop the lookup reference on failure */
    return (uint64_t)(-(uint64_t)ENOMEM);
  }
  __memset(f, 0, sizeof(*f));
  refcount_set(&f->f_count, 1);
  f->type = FD_DEV;
  f->flags = flags;
  f->inode = ip; /* file takes ownership of the lookup +1 reference */

  // Install FD_DEV BEFORE ops->open so callbacks (e.g. pts_open/ptmx_open)
  // can access it via fd_table[fd] and mutate it into FD_TTY in place.
  fd_install(proc->proc->files, fd, f);

  if (ops) {
    f->target_pid = ops->driver_pid;
    // Kernel device: call open callback. Callbacks mutate the FD_DEV file
    // in place (do not replace the pointer), so fd_table[fd] stays valid.
    if (ops->driver_pid == 0 && ops->open) {
      int rc = ops->open(proc, fd);
      if (rc < 0) {
        // Open failed: undo fd installation.
        // Manual cleanup (not file_put) to avoid calling ops->close
        // when ops->open itself failed. Drop the lookup +1 that the file
        // would otherwise have owned, and the §5 fd 引用.
        fd_uninstall(proc->proc->files, fd);
        inode_put(ip);
        dev_ops_put(ops); /* §5: 放 fd 引用(open 未成功,引用不归 fd 持有) */
        kfree(f);
        spin_unlock(fdlk);
        return (uint64_t)(-(uint64_t)(-rc));
      }
    }
    /* evdev 控制节点：ops->ioctl == evdev_control_ioctl 标识。open 后分配
     * input_control_fd 挂入 f->private_data，装 evdev_control_fops（crash
     * 清理由 evdev_control_close 触发）。控制节点无
     * ops->open，故上面分支跳过。*/
    if (ops->driver_pid == 0 && ops->ioctl == evdev_control_ioctl) {
      struct input_control_fd *ctrl =
          (struct input_control_fd *)kmalloc(sizeof(struct input_control_fd));
      if (!ctrl) {
        fd_uninstall(proc->proc->files, fd);
        inode_put(ip);
        dev_ops_put(ops); /* §5: 放 fd 引用 */
        kfree(f);
        spin_unlock(fdlk);
        return (uint64_t)(-(uint64_t)ENOMEM);
      }
      ctrl->manager_pid = proc->pid;
      list_init(&ctrl->instances);
      f->private_data = ctrl;
      f->f_op = &evdev_control_fops;
    }
  }
  spin_unlock(fdlk);
  return (uint64_t)fd;
}

void devtmpfs_cleanup_pid(pid_t pid) {
  struct dev_entry *to_free = NULL; /* pending free 链(复用 e->next 串联) */
  spin_lock(&devtmpfs_lock);
  struct dev_entry **pp = &dev_list;
  while (*pp) {
    struct dev_entry *e = *pp;
    if (e->ip && e->ip->i_priv) {
      struct dev_ops *ops = (struct dev_ops *)e->ip->i_priv;
      if (ops->driver_pid == pid) {
        /* Remove from list */
        *pp = e->next;
        /* Clean up sysfs subtree + uevent_priv + subsys_priv。
         * 顺序:kfree uevent_priv → sysfs_remove_dir(释放 attr 结构体,不释放其
         * priv)→ kfree subsys_priv → put ops(对齐 sys_dev_set_meta 幂等
         * guard)。 */
        if (ops->uevent_priv) {
          kfree(ops->uevent_priv);
          ops->uevent_priv = NULL;
        }
        if (ops->sysfs_dir) {
          sysfs_remove_dir(ops->sysfs_dir);
          ops->sysfs_dir = NULL;
        }
        if (ops->subsys_priv) {
          kfree(ops->subsys_priv);
          ops->subsys_priv = NULL;
        }
        /* §5: 放注册引用(devtmpfs_create 入口 dev_ops_get 取)。不直接 kfree:
         * 此刻可能有别的进程的 fd 持 ops 引用(跨进程持有设备 fd 场景),kfree
         * 会使 ip->i_priv 悬垂,后续 file_put 裸读 i_priv UAF。改 put,归 0
         * (无任何 fd 持引用)才 kfree。user-space driver(driver_pid>0)的
         * kmalloc ops 会归 0;kernel device 的 static ops 永不归 0。*/
        if (ops->driver_pid > 0)
          dev_ops_put(ops);
        /* Free inode */
        inode_put(e->ip);
        e->ip = NULL;
        e->next = to_free; /* 串入 pending(复用 next,节点已脱离 dev_list) */
        to_free = e;
        continue;
      }
    }
    pp = &e->next;
  }
  spin_unlock(&devtmpfs_lock);

  /* 锁外批量 kfree 节点(对齐 tmpfs_unlink 回收纪律) */
  while (to_free) {
    struct dev_entry *n = to_free->next;
    kfree(to_free);
    to_free = n;
  }
}

void devtmpfs_remove(const char *name) {
  struct dev_entry *victim = NULL;
  spin_lock(&devtmpfs_lock);
  struct dev_entry **pp = &dev_list;
  while (*pp) {
    struct dev_entry *e = *pp;
    if (__strcmp(name, e->name) == 0) {
      *pp = e->next;
      if (e->ip)
        inode_put(e->ip);
      e->ip = NULL;
      victim = e;
      break;
    }
    pp = &e->next;
  }
  spin_unlock(&devtmpfs_lock);

  kfree(victim); /* 锁外回收(对齐 tmpfs_unlink 回收纪律) */

  if (victim && devtmpfs_initialized && nl_is_initialized())
    nl_uevent_broadcast("remove", name, "misc");
}

/* ==================== devtmpfs fstype callbacks ==================== */

/* getdents: enumerate direct children of a devtmpfs directory.
 * relpath "" or NULL = root /dev; "dri" = /dev/dri subdir.
 * Scans dev_list + dir_list, matching entries whose name starts with
 * dir + "/" and have no further "/" (direct children only). */
static ssize_t devtmpfs_getdents(struct inode *dir, struct dir_context *ctx) {
  spin_lock(&devtmpfs_lock);

  if (ctx->pos == (uint64_t)-1) {
    spin_unlock(&devtmpfs_lock);
    return 0;
  }

  /* Determine if this is the root /dev directory or a subdirectory. */
  bool is_root = (devtmpfs_root_ip && dir->ino == devtmpfs_root_ip->ino);
  const char *prefix = NULL;
  int prefix_len = 0;

  if (!is_root) {
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
    is_root = (prefix == NULL);
  }

  size_t cur_pos = 0;

  if (is_root) {
    /* Root /dev: dir_list then top-level dev_list entries */
    struct dev_dir *d = dir_list;
    while (d) {
      size_t nl = 0;
      while (d->name[nl])
        nl++;
      uint16_t r = (uint16_t)((sizeof(struct dirent64) + nl + 1 + 7) & ~7);
      if (cur_pos < ctx->pos) {
        cur_pos += r;
        d = d->next;
        continue;
      }
      if (!dir_emit(ctx, d->name, (int)nl, cur_pos, d->ip->ino, DT_DIR))
        goto done;
      cur_pos += r;
      d = d->next;
    }
    struct dev_entry *e = dev_list;
    while (e) {
      size_t nl = 0;
      int has_slash = 0;
      while (e->name[nl]) {
        if (e->name[nl] == '/')
          has_slash = 1;
        nl++;
      }
      if (!has_slash) {
        uint16_t r = (uint16_t)((sizeof(struct dirent64) + nl + 1 + 7) & ~7);
        if (cur_pos < ctx->pos) {
          cur_pos += r;
          e = e->next;
          continue;
        }
        if (!dir_emit(ctx, e->name, (int)nl, cur_pos, e->ip->ino, DT_CHR))
          goto done;
        cur_pos += r;
      }
      e = e->next;
    }
  } else {
    /* Subdirectory: filtered dev_list entries */
    struct dev_entry *e = dev_list;
    while (e) {
      size_t nl = 0;
      while (e->name[nl])
        nl++;
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
          size_t leaf_len = nl - prefix_len - 1;
          uint16_t r =
              (uint16_t)((sizeof(struct dirent64) + leaf_len + 1 + 7) & ~7);
          if (cur_pos < ctx->pos) {
            cur_pos += r;
            e = e->next;
            continue;
          }
          if (!dir_emit(ctx, leaf, (int)leaf_len, cur_pos, e->ip->ino, DT_CHR))
            goto done;
          cur_pos += r;
        }
      }
      e = e->next;
    }
  }

  ctx->pos = (uint64_t)-1; /* EOF: all entries emitted */
done:
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

  /* Find dev_ops by name. devtmpfs_lookup now returns a +1 reference (so the
   * inode cannot be freed by a concurrent remove/cleanup_pid while we read
   * i_priv). We only need the dev_ops pointer; drop the inode reference once
   * ops is extracted. ops itself stays alive for this driver process's
   * lifetime (freed only by devtmpfs_cleanup_pid on the owning driver's
   * exit, which is this process — it can't be reaping itself here). */
  struct inode *ip = devtmpfs_lookup(name);
  if (!ip)
    return (int64_t)-ENOENT;
  struct dev_ops *ops = (struct dev_ops *)ip->i_priv;
  inode_put(ip);
  if (!ops) {
    return (int64_t)-ENOENT;
  }

  /* ↓↓↓ 本方案新增:幂等 guard,防重复调用泄漏 ↓↓↓ */
  /* sysfs_dir 已存在(重复调用):先 kfree uevent_priv 再 sysfs_remove_dir 释放
   * 旧子树(对齐 devtmpfs_cleanup_pid:495-498 的释放逻辑),防重复建 sysfs 文件。
   * sysfs_dir 内 uevent attr 的 priv 指向 uevent_priv,sysfs_remove_dir 只 kfree
   * attr 结构体不释放其 priv,故须先 kfree uevent_priv(subsys_priv 见下,因其
   * show 回调被 remove 遍历无关——remove 不调 show,但保守在 remove 后释放)。 */
  if (ops->uevent_priv) {
    kfree(ops->uevent_priv);
    ops->uevent_priv = NULL;
  }
  if (ops->sysfs_dir) {
    sysfs_remove_dir(ops->sysfs_dir); /* 复用现有 sysfs 子树释放 */
    ops->sysfs_dir = NULL;
  }
  /* subsys_priv 已存在(重复调用):先 kfree 释放旧 props(对齐
   * devtmpfs_cleanup_pid:499-501 的释放逻辑) */
  if (ops->subsys_priv) {
    kfree(ops->subsys_priv);
    ops->subsys_priv = NULL;
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
      /* 可写 uevent 属性(coldplug 用):写 "add" → uevent_store →
       * nl_uevent_broadcast 重广播。devpath=name(DEVPATH 值,如 "input/event0"),
       * subsystem=ops->subsystem。priv 由 dev_ops.uevent_priv 持,在两处 cleanup
       * 的 sysfs_remove_dir 前 kfree(防 UAF)。本轮只写不读(show=NULL),Linux
       * uevent 可读记 todo。拷贝 uevent_attr 模板填 priv(对齐上面 id/ attr 拷贝
       * 模式,uevent_store 保持 static 无需导出符号)。 */
      struct uevent_attr_priv *upriv = kmalloc(sizeof(*upriv));
      if (upriv) {
        __memset(upriv, 0, sizeof(*upriv));
        __strncpy(upriv->devpath, name, sizeof(upriv->devpath) - 1);
        __strncpy(upriv->subsystem, ops->subsystem,
                  sizeof(upriv->subsystem) - 1);
        ops->uevent_priv = upriv;
        struct sysfs_attr *ua = kmalloc(sizeof(*ua));
        if (ua) {
          ua->name = uevent_attr.name;
          ua->priv = upriv;
          ua->show = uevent_attr.show;
          ua->store = uevent_attr.store;
          struct sysfs_node *ufn = sysfs_create_file(devdir, "uevent", ua);
          if (ufn)
            ufn->attr_owned = true;
          else
            kfree(ua);
        }
      }
      ops->sysfs_dir = devdir;
    }
  }

  /* Push uevent (step 2: device ready) */
  if (nl_is_initialized())
    nl_uevent_broadcast("add", name, ops->subsystem);

  return 0;
}

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/bsd/devtmpfs.h"
#include "arch/x64/utils.h"
#include "kernel/bsd/evdev_broker.h"
#include "kernel/bsd/inode.h"
#include "kernel/bsd/kfcntl.h"
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
#include <kernel/bsd/stat_abi.h>
#include <stddef.h>
#include <xos/dirent.h>
#include <xos/errno.h>

#include "kernel/bsd/syscall.h"
#include "kernel/bsd/sysfs.h"

// DRM major (used only for stat device numbers; mirrors virtio_gpu.c DRM_MAJOR
// so devtmpfs need not reverse-include driver headers).
#define DRM_MAJOR_FOR_STAT 226

struct shm;

struct dev_entry {
  char name[32];
  struct inode *ip;
  struct dev_entry *next;
};

// Directory entry for subdirectory support (e.g. "dri").
struct dev_dir {
  char name[32];
  struct inode *ip; // INODE_DIR inode
  struct dev_dir *next;
};

static struct dev_dir *dir_list = NULL;

static struct dev_entry *dev_list = NULL;
static spinlock devtmpfs_lock = SPINLOCK_INIT;

static bool devtmpfs_initialized = false;
// Dedicated root /dev inode — lets getdents distinguish root from
// subdirectories.
static struct inode *devtmpfs_root_ip = NULL;

// §5: dev_ops refcount wrapper (FUSE fuse_conn style).
//   get: take a ref (open path holds the fd ref); refcount_inc BUG_ONs from-0
//   (UAF). put: drop a ref (file_put drops fd ref, cleanup_pid drops
//   registration ref); reaches 0 → kfree.
// Only user-space drivers (driver_pid>0, kmalloc'd ops) reach 0; kernel device
// ops are static, registration ref is permanent, put never kfree's. fd ref
// covers raw i_priv reads in read/write/ioctl/poll, so those paths need no
// get/put.
void dev_ops_get(struct dev_ops *ops) {
  ASSERT(ops);
  refcount_inc(&ops->refcount);
}

void dev_ops_put(struct dev_ops *ops) {
  if (!ops)
    return;
  if (refcount_dec_and_test(&ops->refcount)) {
    // Only user-space driver kmalloc'd ops reach 0; kernel device ops are
    // static and never reach 0. subsys_priv/uevent_priv/sysfs_dir were already
    // cleaned by cleanup_pid.
    kfree(ops);
  }
}

// §5: under devtmpfs_lock, read inode->i_priv and return ops (no ref taken).
// Used by file_put(FD_DEV/FD_TTY) etc.: the §3 borrow-window between a raw
// i_priv read and cleanup_pid's put (ops kfree'd mid-read) is closed by reading
// under the lock. Caller holds the fd ref (taken via dev_ops_get at open), so
// ops won't reach 0 before this fd closes; read result is safe without an extra
// get. close callback + fd ref drop (dev_ops_put) happen outside the lock.
struct dev_ops *dev_ops_peek_by_inode(struct inode *ip) {
  if (!ip)
    return NULL;
  spin_lock(&devtmpfs_lock);
  struct dev_ops *ops = ip->i_priv ? (struct dev_ops *)ip->i_priv : NULL;
  spin_unlock(&devtmpfs_lock);
  return ops;
}

// Reverse map: device inode → registered /dev/<name>. Used by procfs
// /proc/self/fd/N readlink so a char/block device fd (FD_DEV) resolves to its
// real /dev path (e.g. /dev/serial, /dev/console) instead of an anon_inode
// magic string — matching Linux, where readlink on a device fd returns the
// device path and ttyname_r's dev/ino cross-check then succeeds. Returns NULL
// if the inode is not a registered devtmpfs device (caller falls back). The
// returned pointer is owned by the dev_entry and stable while the device is
// registered; caller must not free it and should copy out under no lock if it
// outlives the call, so we copy into the caller's buffer here instead.
const char *devtmpfs_name_by_inode(struct inode *ip) {
  if (!ip)
    return NULL;
  const char *name = NULL;
  spin_lock(&devtmpfs_lock);
  for (struct dev_entry *e = dev_list; e; e = e->next) {
    if (e->ip == ip) {
      name = e->name;
      break;
    }
  }
  spin_unlock(&devtmpfs_lock);
  return name;
}

// Forward: devtmpfs_iget defined later (after devtmpfs_get_or_create_dir),
// but devtmpfs_init / devtmpfs_get_or_create_dir call it.
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

  // Create dedicated root inode for /dev (distinguished from subdirectories).
  // Must be outside the lock because inode_create may allocate.
  devtmpfs_root_ip = devtmpfs_iget(INODE_DIR);
  devtmpfs_initialized = true;
  printk(LOG_INFO, "devtmpfs_init: done\n");
}

// Find or create a subdirectory dev_dir entry by name (no slash in name)
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
  nd->ip->i_priv =
      nd; // I5: subdir inode back-pointer to dev_dir, lookup uses prefix
  nd->next = dir_list;
  dir_list = nd;
  return nd;
}

int devtmpfs_mkdir(const char *name) {
  if (!name || !name[0] || __strchr(name, '/'))
    return -EINVAL;
  int len = (int)__strlen(name);
  spin_lock(&devtmpfs_lock);
  struct dev_dir *dir = devtmpfs_get_or_create_dir(name, len);
  spin_unlock(&devtmpfs_lock);
  return dir ? 0 : -ENOMEM;
}

// devtmpfs_iget: wraps inode_create + sets i_op. devtmpfs inodes are kept
// resident by a base ref held via the dev_list/dev_dir kmalloc'd list node's
// ip.
static const struct inode_operations devtmpfs_dir_iop;
static const struct inode_operations devtmpfs_dev_iop;

static struct inode *devtmpfs_iget(int type) {
  struct inode *ip = inode_create(0, type, 0, 0, 0, 0);
  if (!ip)
    return NULL;
  ip->i_op = (type == INODE_DIR) ? &devtmpfs_dir_iop : &devtmpfs_dev_iop;
  return ip;
}

// devtmpfs_dir_lookup: find a direct child named `name` in dir; returns +1
// inode or NULL. I4(a) decision: dev_list stores full names (e.g. "dri/card0"),
// path_walk walks one segment at a time, so we use the dir identity's prefix to
// build the full name and compare. Root (dir->i_priv==NULL) flat-matches
// top-level entries; subdir (i_priv==dev_dir*) matches "prefix/name". Honors
// the +1 inode_get contract (fixes the old devtmpfs_lookup borrow-without-get
// UAF).
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
      // Root: only match top-level (full name without '/').
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
      // Subdir: match "prefix/name".
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
  // Subdirs don't nest further dev_dirs (currently only one level is
  // supported); don't scan dir_list here.
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

// devtmpfs_getattr: filled from ip fields. Root st_ino=ip->ino (fixes old
// hardcoded 0). st_rdev=ip->ino is a device-number architecture gap (todo §3.5,
// untouched). S08: report real ip->mode/uid/gid (devtmpfs inodes are
// kernel-built, owner defaults to root=0; mode set to S_IFCHR|0600 in
// devtmpfs_create, dirs default to 0040755).
static int devtmpfs_getattr(struct inode *ip, struct kstat *ks) {
  __memset(ks, 0, sizeof(*ks));
  if (ip->type == INODE_DIR) {
    ks->st_mode = ip->mode ? ip->mode : 0040755;
  } else {
    ks->st_mode = ip->mode ? ip->mode : (0020000 | 0600); // S_IFCHR | 0600
    // Device number: DRM devices return the real makedev(226, minor) (libdrm
    // uses fstat.st_rdev to distinguish render/primary — see
    // drmGetNodeTypeFromFd); other devices keep =ino (architecture gap, todo
    // §3.5, no consumer depends on it). Subdir inodes have i_priv = dev_dir*,
    // but this else branch only runs for char device inodes whose i_priv is
    // always a dev_ops*.
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
  // Timestamps (Q5 in-memory): getattr splits ns into sec/nsec.
  ks->st_atim.tv_sec = (int64_t)(ip->atime / 1000000000ULL);
  ks->st_atim.tv_nsec = (int64_t)(ip->atime % 1000000000ULL);
  ks->st_mtim.tv_sec = (int64_t)(ip->mtime / 1000000000ULL);
  ks->st_mtim.tv_nsec = (int64_t)(ip->mtime % 1000000000ULL);
  ks->st_ctim.tv_sec = (int64_t)(ip->ctime / 1000000000ULL);
  ks->st_ctim.tv_nsec = (int64_t)(ip->ctime % 1000000000ULL);
  return 0;
}

static const struct inode_operations devtmpfs_dir_iop = {
    .lookup = devtmpfs_dir_lookup,
    .getattr = devtmpfs_getattr,
};

static const struct inode_operations devtmpfs_dev_iop = {
    .getattr = devtmpfs_getattr,
};

// devtmpfs_mount_root: returns /dev root inode (with inode_get taken).
static struct inode *devtmpfs_mount_root(struct mount_entry *m) {
  (void)m;
  if (!devtmpfs_root_ip)
    return NULL;
  return inode_get(devtmpfs_root_ip);
}

struct inode *devtmpfs_lookup(const char *name) {
  // relpath from vfs_resolve has no /dev/ prefix; entries store paths relative
  // to /dev (e.g. "serial", "dri/card0"). Empty string = root /dev directory —
  // return the dedicated root inode.
  if (name[0] == '\0') {
    if (devtmpfs_root_ip) {
      inode_get(devtmpfs_root_ip);
      return devtmpfs_root_ip;
    }
    return NULL;
  }

  // If path contains '/', split into dir + leaf.
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
    // Lookup leaf inside dir: match by full path (stored entry.name includes
    // the dir/ prefix). Return a +1 reference taken under the lock so the inode
    // cannot be freed (by a concurrent devtmpfs_remove / cleanup_pid) between
    // this unlock and the caller's first deref of ip->i_priv — that borrow
    // window was bug.md §3 (UAF → ip->i_priv=0x10 → #PF). Matches the
    // devtmpfs_dir_lookup +1 contract; callers inode_put when done.
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
  // No slash: flat lookup — search devices first, then directories.
  // Same +1 contract as the slash path (see comment above).
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
  // Check directories.
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

  // Check if already exists (full path). devtmpfs_lookup now returns a +1
  // reference, so drop it before deciding.
  struct inode *existing = devtmpfs_lookup(name);
  if (existing) {
    inode_put(existing);
    return -EEXIST;
  }

  // If path contains '/', create the subdirectory first.
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

  // Create inode.
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

  // Fill entry — store full path including any '/'.
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

  // §5: registration ref (held by the dev_list entry). ops arrives with
  // refcount 0 (static/embedded via zero-init, kmalloc via __memset(0)); this
  // establishes the first ref meaning "registered". Do NOT use dev_ops_get():
  // refcount_inc treats 0→1 as UAF and BUG_ONs — that guard is for "take extra
  // ref" paths (open/peek), not for from-0 bootstrapping. If the same ops is
  // registered multiple times (e.g. random/urandom share random_ops), from the
  // 2nd call refcount is already ≥1, so dev_ops_get +1 works normally; the
  // first (==0) uses refcount_set. cleanup_pid drops this ref; reaches 0 (with
  // no fd refs) → kfree.
  if (refcount_read(&ops->refcount) == 0)
    refcount_set(&ops->refcount, 1);
  else
    dev_ops_get(ops);

  spin_unlock(&devtmpfs_lock);
  printk(LOG_INFO, "devtmpfs: created /dev/%s\n", name);

  // Broadcast uevent only for kernel devices (user-space drivers push via
  // SYS_DEV_SET_META after metadata is set — design 3.3.2 step 2).
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

  // Handle directories: create FD_DIR (not FD_DEV) so getdents works.
  // Also set ip->mount so mount_of_inode() finds the devtmpfs fstype.
  if (ip->type == INODE_DIR) {
    ip->mount = m;
    files *fs = proc->proc->files;
    spinlock *fdlk = &fs->fd_lock;
    spin_lock(fdlk);
    int fd = alloc_fd(fs, 0);
    if (fd < 0) {
      spin_unlock(fdlk);
      inode_put(ip); // drop the lookup reference on failure
      return (uint64_t)(-(uint64_t)EMFILE);
    }
    struct file *f = kmalloc(sizeof(struct file));
    if (!f) {
      spin_unlock(fdlk);
      inode_put(ip); // drop the lookup reference on failure
      return (uint64_t)(-(uint64_t)ENOMEM);
    }
    __memset(f, 0, sizeof(*f));
    refcount_set(&f->f_count, 1);
    f->type = FD_DIR;
    f->flags = O_RDONLY;
    f->inode = ip; // file takes ownership of the lookup +1 reference
    f->offset = 0;
    fd_install(fs, fd, f);
    spin_unlock(fdlk);
    return (uint64_t)fd;
  }

  // Device open path.
  // §5: take the ops (fd) ref in a separate devtmpfs_lock critical section
  // BEFORE taking fdlk. fdlk doesn't protect against devtmpfs_cleanup_pid
  // (which holds devtmpfs_lock); a raw i_priv read inside fdlk would race
  // cleanup_pid's put (§3 borrow-window: ops kfree'd before we take the ref).
  // Holding the fd ref keeps ops alive for this fd's whole lifetime, so raw
  // i_priv reads in read/write/ioctl/poll are safe. The fd ref is dropped in
  // file_put(FD_DEV).
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
      dev_ops_put(ops); // §5: drop the just-taken fd ref
    inode_put(ip);      // drop the lookup reference on failure
    return (uint64_t)(-(uint64_t)EMFILE);
  }

  struct file *f = kmalloc(sizeof(struct file));
  if (!f) {
    spin_unlock(fdlk);
    if (ops)
      dev_ops_put(ops); // §5: drop the just-taken fd ref
    inode_put(ip);      // drop the lookup reference on failure
    return (uint64_t)(-(uint64_t)ENOMEM);
  }
  __memset(f, 0, sizeof(*f));
  refcount_set(&f->f_count, 1);
  f->type = FD_DEV;
  f->flags = flags;
  f->inode = ip; // file takes ownership of the lookup +1 reference

  // Install FD_DEV BEFORE ops->open so callbacks (e.g. pts_open/ptmx_open) can
  // access it via fd_table[fd] and mutate it into FD_TTY in place.
  fd_install(proc->proc->files, fd, f);

  if (ops) {
    f->target_pid = ops->driver_pid;
    // Kernel device: call open callback. Callbacks mutate the FD_DEV file in
    // place (do not replace the pointer), so fd_table[fd] stays valid.
    if (ops->driver_pid == 0 && (ops->open_file || ops->open)) {
      int rc = ops->open_file ? ops->open_file(proc, f) : ops->open(proc, fd);
      if (rc < 0) {
        // Open failed: undo fd installation. Manual cleanup (not file_put) to
        // avoid calling ops->close when ops->open itself failed. Drop the
        // lookup +1 that the file would otherwise have owned, and the §5 fd
        // ref.
        fd_uninstall(proc->proc->files, fd);
        inode_put(ip);
        dev_ops_put(ops); // §5: drop fd ref (open failed, ref not owned by fd)
        kfree(f);
        spin_unlock(fdlk);
        return (uint64_t)(-(uint64_t)(-rc));
      }
    }
    // evdev control node: identified by ops->ioctl == evdev_control_ioctl.
    // After open, allocate input_control_fd into f->private_data and install
    // evdev_control_fops (crash cleanup triggered by evdev_control_close). The
    // control node has no ops->open, so the branch above was skipped.
    if (ops->driver_pid == 0 && ops->ioctl == evdev_control_ioctl) {
      struct input_control_fd *ctrl =
          (struct input_control_fd *)kmalloc(sizeof(struct input_control_fd));
      if (!ctrl) {
        fd_uninstall(proc->proc->files, fd);
        inode_put(ip);
        dev_ops_put(ops); // §5: drop fd ref
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
  struct dev_entry *to_free =
      NULL; // pending-free list (reuses e->next to chain)
  spin_lock(&devtmpfs_lock);
  struct dev_entry **pp = &dev_list;
  while (*pp) {
    struct dev_entry *e = *pp;
    if (e->ip && e->ip->i_priv) {
      struct dev_ops *ops = (struct dev_ops *)e->ip->i_priv;
      if (ops->driver_pid == pid) {
        // Remove from list.
        *pp = e->next;
        // Clean up sysfs subtree + uevent_priv + subsys_priv.
        // Order: kfree uevent_priv → sysfs_remove_dir (frees attr structs, not
        // their priv) → kfree subsys_priv → put ops (mirrors sys_dev_set_meta
        // idempotent guard).
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
        // §5: drop registration ref (taken by dev_ops_get at devtmpfs_create).
        // Do NOT kfree directly: another process may hold an fd ref to ops
        // (cross-process device fd); kfree here would dangle ip->i_priv and the
        // subsequent file_put raw i_priv read would UAF. Use put; reaches 0 (no
        // fd refs) → kfree. User-space driver (driver_pid>0) kmalloc'd ops
        // reach 0; kernel device static ops never do.
        if (ops->driver_pid > 0)
          dev_ops_put(ops);
        // Free inode.
        inode_put(e->ip);
        e->ip = NULL;
        e->next = to_free; // chain into pending (next reused; node already off
                           // dev_list)
        to_free = e;
        continue;
      }
    }
    pp = &e->next;
  }
  spin_unlock(&devtmpfs_lock);

  // Batch kfree nodes outside the lock (mirrors tmpfs_unlink reclaim
  // discipline).
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

  kfree(victim); // reclaim outside the lock (mirrors tmpfs_unlink discipline)

  if (victim && devtmpfs_initialized && nl_is_initialized())
    nl_uevent_broadcast("remove", name, "misc");
}

// ==================== devtmpfs fstype callbacks ====================

// getdents: enumerate direct children of a devtmpfs directory.
// relpath "" or NULL = root /dev; "dri" = /dev/dri subdir.
// Scans dev_list + dir_list, matching entries whose name starts with dir + "/"
// and have no further "/" (direct children only).
static ssize_t devtmpfs_getdents(struct inode *dir, struct dir_context *ctx) {
  spin_lock(&devtmpfs_lock);

  if (ctx->pos == (uint64_t)-1) {
    spin_unlock(&devtmpfs_lock);
    return 0;
  }

  // Determine if this is the root /dev directory or a subdirectory.
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

  // Synthetic "." and ".." precede real children (in-memory fs has no on-disk
  // dot entries). devtmpfs tracks no parent link, so both point at this
  // directory (root-like ".." → self, matching FAT-root behavior).
  {
    size_t nl = 1; // "."
    uint16_t r = (uint16_t)((sizeof(struct dirent64) + nl + 1 + 7) & ~7);
    if (cur_pos >= ctx->pos &&
        !dir_emit(ctx, ".", (int)nl, cur_pos, dir->ino, DT_DIR))
      goto done;
    cur_pos += r;
  }
  {
    size_t nl = 2; // ".."
    uint16_t r = (uint16_t)((sizeof(struct dirent64) + nl + 1 + 7) & ~7);
    if (cur_pos >= ctx->pos &&
        !dir_emit(ctx, "..", (int)nl, cur_pos, dir->ino, DT_DIR))
      goto done;
    cur_pos += r;
  }

  if (is_root) {
    // Root /dev: dir_list then top-level dev_list entries.
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
    // Subdirectory: filtered dev_list entries.
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

  ctx->pos = (uint64_t)-1; // EOF: all entries emitted
done:
  spin_unlock(&devtmpfs_lock);
  return (ssize_t)ctx->written;
}

// R1 stub: returns NULL. R3 (plan_vfs1.md) replaces it via devtmpfs_mount_root.
struct fstype devtmpfs_fstype = {
    .name = "devtmpfs",
    .mount_root = devtmpfs_mount_root,
    .getdents = devtmpfs_getdents,
};

// sys_dev_set_meta(name, subsystem, devtype, props) — SYS_DEV_SET_META.
// Sets device metadata + builds sysfs subtree + pushes uevent.
// Step 2 of two-step registration (design 3.3.2).
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

  // Find dev_ops by name. devtmpfs_lookup now returns a +1 reference (so the
  // inode cannot be freed by a concurrent remove/cleanup_pid while we read
  // i_priv). We only need the dev_ops pointer; drop the inode reference once
  // ops is extracted. ops itself stays alive for this driver process's lifetime
  // (freed only by devtmpfs_cleanup_pid on the owning driver's exit, which is
  // this process — it can't be reaping itself here).
  struct inode *ip = devtmpfs_lookup(name);
  if (!ip)
    return (int64_t)-ENOENT;
  struct dev_ops *ops = (struct dev_ops *)ip->i_priv;
  inode_put(ip);
  if (!ops) {
    return (int64_t)-ENOENT;
  }

  // Idempotent guard against repeated calls (prevents leaks). If sysfs_dir
  // already exists (repeat call): kfree uevent_priv first, then
  // sysfs_remove_dir frees the old subtree (mirrors
  // devtmpfs_cleanup_pid:495-498) to prevent duplicate sysfs files.
  // sysfs_remove_dir frees only the attr structs, not their priv, so
  // uevent_priv must be freed first (subsys_priv freed after remove for
  // conservatism — remove doesn't call show, but keep the order conservative).
  if (ops->uevent_priv) {
    kfree(ops->uevent_priv);
    ops->uevent_priv = NULL;
  }
  if (ops->sysfs_dir) {
    sysfs_remove_dir(ops->sysfs_dir); // reuse existing sysfs subtree release
    ops->sysfs_dir = NULL;
  }
  // subsys_priv already present (repeat call): kfree the old props (mirrors
  // devtmpfs_cleanup_pid:499-501).
  if (ops->subsys_priv) {
    kfree(ops->subsys_priv);
    ops->subsys_priv = NULL;
  }

  // Fill subsystem/devtype.
  __strncpy(ops->subsystem, subsystem, 7);
  ops->subsystem[7] = '\0';
  __strncpy(ops->devtype, devtype, 7);
  ops->devtype[7] = '\0';

  // Copy props if provided.
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

  // Build sysfs subtree for input devices with props.
  if (__strcmp(ops->subsystem, "input") == 0 && iprops) {
    const char *slash = name;
    while (*slash && *slash != '/')
      slash++;
    const char *basename = (*slash == '/') ? slash + 1 : name;

    struct sysfs_node *cls = sysfs_class_dir("input");
    struct sysfs_node *devdir = sysfs_create_dir(cls, basename);
    if (devdir) {
      // Per-device attr copies: const templates have no priv; we need priv =
      // iprops so show callbacks read this device's properties. (Shared mutable
      // attrs would corrupt across multiple devices.)
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
      // Writable uevent attr (used by coldplug): writing "add" → uevent_store →
      // nl_uevent_broadcast re-broadcasts. devpath=name (DEVPATH value, e.g.
      // "input/event0"), subsystem=ops->subsystem. priv held by
      // dev_ops.uevent_priv, freed in both cleanup paths before
      // sysfs_remove_dir (prevents UAF). This round is write-only (show=NULL);
      // making uevent readable is a todo (Linux parity). Copy the uevent_attr
      // template and fill priv (mirrors the id/ attr copy pattern above;
      // uevent_store stays static, no symbol export needed).
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

  // Push uevent (step 2: device ready).
  if (nl_is_initialized())
    nl_uevent_broadcast("add", name, ops->subsystem);

  return 0;
}

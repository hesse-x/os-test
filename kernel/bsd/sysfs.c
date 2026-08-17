/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#include "kernel/bsd/sysfs.h"

#include "arch/x64/utils.h"
#include <xos/dirent.h>
struct xtask;
#include "kernel/bsd/fops.h"
#include "kernel/bsd/inode.h"
#include "kernel/bsd/kfcntl.h"
#include "kernel/bsd/mount.h"
#include "kernel/bsd/netlink.h"
#include "kernel/bsd/types.h"
#include "kernel/xcore/kpi.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mm_types.h"
#include "kernel/xcore/spinlock.h"
#include <kernel/bsd/stat_abi.h>
#include <xos/errno.h>

// ===== sysfs node tree =====
static struct sysfs_node *sysfs_root;
static spinlock sysfs_lock = SPINLOCK_INIT;
static uint32_t sysfs_ino_counter = 0x10000;

static struct sysfs_node *node_alloc(const char *name, bool is_dir) {
  struct sysfs_node *n = kmalloc(sizeof(struct sysfs_node));
  if (!n)
    return NULL;
  __memset(n, 0, sizeof(*n));
  __strncpy(n->name, name, 31);
  n->name[31] = '\0';
  n->is_dir = is_dir;
  n->ino = sysfs_ino_counter++;
  return n;
}

struct sysfs_node *sysfs_create_dir(struct sysfs_node *parent,
                                    const char *name) {
  if (!parent)
    parent = sysfs_root;
  if (!parent)
    return NULL;
  spin_lock(&sysfs_lock);
  // Check for an existing entry.
  for (struct sysfs_node *c = parent->children; c; c = c->sibling) {
    if (__strcmp(c->name, name) == 0) {
      spin_unlock(&sysfs_lock);
      return c;
    }
  }
  struct sysfs_node *n = node_alloc(name, true);
  if (!n) {
    spin_unlock(&sysfs_lock);
    return NULL;
  }
  n->parent = parent;
  n->sibling = parent->children;
  parent->children = n;
  spin_unlock(&sysfs_lock);
  return n;
}

struct sysfs_node *sysfs_create_file(struct sysfs_node *parent,
                                     const char *name,
                                     const struct sysfs_attr *attr) {
  if (!parent)
    parent = sysfs_root;
  if (!parent)
    return NULL;
  spin_lock(&sysfs_lock);
  struct sysfs_node *n = node_alloc(name, false);
  if (!n) {
    spin_unlock(&sysfs_lock);
    return NULL;
  }
  n->attr = (struct sysfs_attr *)attr;
  n->parent = parent;
  n->sibling = parent->children;
  parent->children = n;
  spin_unlock(&sysfs_lock);
  return n;
}

struct sysfs_node *sysfs_create_symlink(struct sysfs_node *parent,
                                        const char *name, const char *target) {
  if (!parent)
    parent = sysfs_root;
  if (!parent || !target)
    return NULL;

  size_t target_len = __strlen(target);
  char *target_copy = kmalloc(target_len + 1);
  if (!target_copy)
    return NULL;
  __memcpy(target_copy, target, target_len + 1);

  spin_lock(&sysfs_lock);
  for (struct sysfs_node *c = parent->children; c; c = c->sibling) {
    if (__strcmp(c->name, name) == 0) {
      spin_unlock(&sysfs_lock);
      kfree(target_copy);
      return c->is_symlink ? c : NULL;
    }
  }
  struct sysfs_node *n = node_alloc(name, false);
  if (!n) {
    spin_unlock(&sysfs_lock);
    kfree(target_copy);
    return NULL;
  }
  n->is_symlink = true;
  n->symlink_target = target_copy;
  n->parent = parent;
  n->sibling = parent->children;
  parent->children = n;
  spin_unlock(&sysfs_lock);
  return n;
}

int sysfs_node_set_owner(struct sysfs_node *node, void *owner,
                         const struct vma_owner_ops *ops) {
  if (!node || !owner || !ops || !ops->get || !ops->put || node->owner)
    return -EINVAL;
  ops->get(owner);
  node->owner = owner;
  node->owner_ops = ops;
  return 0;
}

static void sysfs_inode_release(struct inode *ip, void *arg) {
  (void)ip;
  struct sysfs_node *n = arg;
  if (n->attr_priv_owned && n->attr)
    kfree(n->attr->priv);
  if (n->attr_owned)
    kfree(n->attr);
  kfree(n->symlink_target);
  if (n->owner_ops)
    n->owner_ops->put(n->owner);
  kfree(n);
}

static void sysfs_free_subtree(struct sysfs_node *n) {
  struct sysfs_node *child = n->children;
  while (child) {
    struct sysfs_node *next = child->sibling;
    sysfs_free_subtree(child);
    child = next;
  }
  n->removed = true;
  n->parent = NULL;
  n->children = NULL;
  n->sibling = NULL;
  if (n->ip) {
    inode_put(n->ip);
    return;
  }
  sysfs_inode_release(NULL, n);
}

void sysfs_remove_dir(struct sysfs_node *dir) {
  if (!dir || !dir->parent)
    return;
  spin_lock(&sysfs_lock);
  // Detach from the parent's children list.
  struct sysfs_node **pp = &dir->parent->children;
  while (*pp && *pp != dir)
    pp = &(*pp)->sibling;
  if (*pp)
    *pp = dir->sibling;
  sysfs_free_subtree(dir);
  spin_unlock(&sysfs_lock);
}

struct sysfs_node *sysfs_class_dir(const char *subsystem) {
  static struct sysfs_node *class_dir;
  if (!class_dir)
    class_dir = sysfs_create_dir(sysfs_root, "class");
  return sysfs_create_dir(class_dir, subsystem);
}

// ===== lookup: walk the tree component by component =====
static struct sysfs_node *sysfs_walk(const char *relpath) {
  struct sysfs_node *cur = sysfs_root;
  const char *p = relpath;
  while (*p && cur) {
    const char *slash = p;
    while (*slash && *slash != '/')
      slash++;
    int len = slash - p;
    if (len == 0) {
      p = slash + 1;
      continue;
    }
    struct sysfs_node *found = NULL;
    spin_lock(&sysfs_lock);
    for (struct sysfs_node *c = cur->children; c; c = c->sibling) {
      if (__strlen(c->name) == (size_t)len && __memcmp(c->name, p, len) == 0) {
        found = c;
        break;
      }
    }
    spin_unlock(&sysfs_lock);
    cur = found;
    p = (*slash == '/') ? slash + 1 : slash;
  }
  return cur;
}

static const struct inode_operations sysfs_dir_iop;
static const struct inode_operations sysfs_file_iop;
static const struct inode_operations sysfs_lnk_iop;
static struct super_block sysfs_sb;

static struct inode *sysfs_node_to_inode(struct sysfs_node *n) {
  if (n->ip) {
    n->ip->i_op = n->is_symlink
                      ? &sysfs_lnk_iop
                      : (n->is_dir ? &sysfs_dir_iop : &sysfs_file_iop);
    return inode_get(n->ip);
  }
  int type =
      n->is_symlink ? INODE_LNK : (n->is_dir ? INODE_DIR : INODE_REGULAR);
  struct inode *ip = inode_create(&sysfs_sb, n->ino, type, 0);
  if (!ip)
    return NULL;
  // S08: sysfs attribute files are read-only 0100444 (inode_create defaults to
  // 0100644 writable, which doesn't match sysfs semantics); directories are
  // 0040755. Owner defaults to 0 (root) — kernel-created.
  ip->mode = n->is_symlink ? 0120777 : (n->is_dir ? 0040755 : 0100444);
  ip->i_priv = (n->is_dir || n->is_symlink) ? (void *)n : (void *)n->attr;
  ip->release = sysfs_inode_release;
  ip->release_arg = n;
  ip->i_op = n->is_symlink ? &sysfs_lnk_iop
                           : (n->is_dir ? &sysfs_dir_iop : &sysfs_file_iop);
  ip->i_fop = type == INODE_REGULAR ? &sysfs_fops : NULL;
  if (n->is_symlink)
    ip->size = __strlen(n->symlink_target);
  n->ip = inode_get(ip);
  return ip;
}

// sysfs_dir_lookup: look up `name` under directory inode `dir`; returns +1
// inode or NULL.
static struct inode *sysfs_dir_lookup(struct inode *dir, const char *name) {
  struct sysfs_node *parent = (struct sysfs_node *)dir->i_priv;
  if (!parent || !parent->is_dir)
    return NULL;
  int namelen = 0;
  while (name[namelen])
    namelen++;
  spin_lock(&sysfs_lock);
  struct sysfs_node *found = NULL;
  for (struct sysfs_node *c = parent->children; c; c = c->sibling) {
    if (__strlen(c->name) == (size_t)namelen &&
        __memcmp(c->name, name, namelen) == 0) {
      found = c;
      break;
    }
  }
  spin_unlock(&sysfs_lock);
  if (!found)
    return NULL;
  return sysfs_node_to_inode(found);
}

// sysfs_getattr: fill from ip fields (does not deref i_priv, avoiding a fragile
// dir/node vs file/attr distinction). S08: reports the real ip->mode/uid/gid
// (sysfs inodes are kernel-created, owner defaults to 0).
static int sysfs_getattr(struct inode *ip, struct kstat *ks) {
  __memset(ks, 0, sizeof(*ks));
  ks->st_ino = ip->ino;
  ks->st_mode = ip->mode;
  ks->st_uid = ip->uid;
  ks->st_gid = ip->gid;
  ks->st_nlink = 1;
  ks->st_size = 0;
  ks->st_blksize = 4096;
  // Timestamps (Q5, in-memory): getattr reads ns and splits into sec/nsec.
  ks->st_atim.tv_sec = ip->atime.tv_sec;
  ks->st_atim.tv_nsec = ip->atime.tv_nsec;
  ks->st_mtim.tv_sec = ip->mtime.tv_sec;
  ks->st_mtim.tv_nsec = ip->mtime.tv_nsec;
  ks->st_ctim.tv_sec = ip->ctime.tv_sec;
  ks->st_ctim.tv_nsec = ip->ctime.tv_nsec;
  return 0;
}

static const struct inode_operations sysfs_dir_iop = {
    .lookup = sysfs_dir_lookup,
    .getattr = sysfs_getattr,
};

static const struct inode_operations sysfs_file_iop = {
    .getattr = sysfs_getattr,
};

static int sysfs_symlink_readlink(struct inode *ip, char *buf, size_t bufsiz) {
  struct sysfs_node *n = (struct sysfs_node *)ip->i_priv;
  if (!n || !n->is_symlink || !n->symlink_target)
    return -EIO;
  size_t len = __strlen(n->symlink_target);
  if (len > bufsiz)
    len = bufsiz;
  __memcpy(buf, n->symlink_target, len);
  return (int)len;
}

static const struct inode_operations sysfs_lnk_iop = {
    .readlink = sysfs_symlink_readlink,
    .getattr = sysfs_getattr,
};

// sysfs_mount_root: returns the /sys root inode (with inode_get taken).
static struct inode *sysfs_mount_root(struct mount_entry *m) {
  (void)m;
  return sysfs_node_to_inode(sysfs_root);
}

struct inode *sysfs_lookup(const char *relpath) {
  if (!sysfs_root)
    return NULL;
  if (relpath[0] == '\0')
    return sysfs_node_to_inode(sysfs_root);
  struct sysfs_node *n = sysfs_walk(relpath);
  if (!n)
    return NULL;
  return sysfs_node_to_inode(n);
}

ssize_t sysfs_getdents(struct inode *dir, struct dir_context *ctx) {
  struct sysfs_node *n = (struct sysfs_node *)dir->i_priv;
  if (!n || !n->is_dir)
    return 0;
  spin_lock(&sysfs_lock);
  if (ctx->pos == (uint64_t)-1) {
    spin_unlock(&sysfs_lock);
    return 0;
  }
  size_t cur_pos = 0;
  // Synthetic "." and ".." precede real children (in-memory fs has no on-disk
  // dot entries). sysfs_node.parent is NULL at the root, so root ".." → self.
  uint64_t parent_ino = n->parent ? n->parent->ino : n->ino;
  {
    size_t nl = 1; // "."
    uint16_t r = (uint16_t)((sizeof(struct dirent64) + nl + 1 + 7) & ~7);
    if (cur_pos >= ctx->pos &&
        !dir_emit(ctx, ".", (int)nl, cur_pos, n->ino, DT_DIR))
      goto done;
    cur_pos += r;
  }
  {
    size_t nl = 2; // ".."
    uint16_t r = (uint16_t)((sizeof(struct dirent64) + nl + 1 + 7) & ~7);
    if (cur_pos >= ctx->pos &&
        !dir_emit(ctx, "..", (int)nl, cur_pos, parent_ino, DT_DIR))
      goto done;
    cur_pos += r;
  }
  struct sysfs_node *c = n->children;
  while (c) {
    size_t nl = __strlen(c->name);
    unsigned dt = c->is_symlink ? DT_LNK : (c->is_dir ? DT_DIR : DT_REG);
    uint16_t r = (uint16_t)((sizeof(struct dirent64) + nl + 1 + 7) & ~7);
    if (cur_pos < ctx->pos) {
      cur_pos += r;
      c = c->sibling;
      continue;
    }
    if (!dir_emit(ctx, c->name, (int)nl, cur_pos, c->ino, dt))
      goto done;
    cur_pos += r;
    c = c->sibling;
  }
  ctx->pos = (uint64_t)-1; // EOF: all entries emitted
done:
  spin_unlock(&sysfs_lock);
  return (ssize_t)ctx->written;
}

int sysfs_stat(const char *relpath, struct kstat *ks) {
  struct sysfs_node *n;
  if (relpath[0] == '\0')
    n = sysfs_root;
  else
    n = sysfs_walk(relpath);
  if (!n)
    return -ENOENT;
  __memset(ks, 0, sizeof(*ks));
  ks->st_mode = n->is_symlink ? 0120777 : (n->is_dir ? 0040755 : 0100444);
  ks->st_ino = n->ino;
  ks->st_nlink = 1;
  ks->st_size = 0;
  ks->st_blksize = 512;
  return 0;
}

// ===== uevent store callback (mirrors Linux kobject uevent attribute writes)
// =====
// uevent_store: writing "add" to /sys/.../uevent re-broadcasts the uevent. It
// goes through the same netlink path (nl_uevent_broadcast) as the original
// broadcast (devtmpfs.c device_create/device_set_meta), so coldplug and hotplug
// share a path. This round accepts only "add" (Linux accepts add/remove/change;
// todo).
static ssize_t uevent_store(const char *buf, size_t len, void *priv) {
  struct uevent_attr_priv *p = (struct uevent_attr_priv *)priv;
  if (!p)
    return -EIO;
  // Parse the action: up to the first '\n' or '\0'.
  size_t alen = 0;
  while (alen < len && buf[alen] != '\n' && buf[alen] != '\0')
    alen++;
  if (alen == 3 && __memcmp(buf, "add", 3) == 0) {
    nl_uevent_broadcast("add", p->devpath, p->subsystem);
    return (ssize_t)len;
  }
  // This round only input coldplug uses "add"; remove/change are todo.
  return -EINVAL;
}

// ===== sysfs_fops.write =====
static ssize_t sysfs_file_write(struct xtask *proc, struct file *f,
                                const void *buf, size_t count) {
  (void)proc;
  struct inode *ip = f->inode;
  if (!ip || !ip->i_priv)
    return -ENODEV;
  // sysfs_fops serves both read and write: reject O_RDONLY writes.
  if (!(f->flags & (O_WRONLY | O_RDWR)))
    return -EBADF;
  struct sysfs_attr *attr = (struct sysfs_attr *)ip->i_priv;
  if (!attr->store)
    return -EIO;
  if (count > 4096)
    count = 4096;
  char kbuf[4096];
  if (copy_from_user(kbuf, buf, count))
    return -EFAULT;
  return attr->store(kbuf, count, attr->priv);
}

// ===== sysfs_fops.read =====
static ssize_t sysfs_file_read(struct xtask *proc, struct file *f, void *buf,
                               size_t count) {
  (void)proc;
  struct inode *ip = f->inode;
  if (!ip || !ip->i_priv)
    return -ENODEV;
  struct sysfs_attr *attr = (struct sysfs_attr *)ip->i_priv;
  if (!attr->show)
    return 0;
  if (count == 0)
    return 0;
  char kbuf[4096];
  ssize_t n = attr->show(kbuf, sizeof(kbuf), attr->priv);
  if (n < 0)
    return n;
  if (n > (ssize_t)sizeof(kbuf))
    n = (ssize_t)sizeof(kbuf);
  if (f->offset >= (uint64_t)n)
    return 0;

  size_t available = (size_t)n - (size_t)f->offset;
  if (count > available)
    count = available;
  if (copy_to_user(buf, kbuf + f->offset, count))
    return -EFAULT;
  f->offset += count;
  return (ssize_t)count;
}

const struct file_operations sysfs_fops = {
    .read = sysfs_file_read,
    .write = sysfs_file_write,
};

// ===== fstype =====
// R1 stub: returns NULL. R3 (plan_vfs1.md) replaces this with sysfs_mount_root.
struct fstype sysfs_fstype = {
    .name = "sysfs",
    .mount_root = sysfs_mount_root,
    .getdents = sysfs_getdents,
};

// ===== initialization =====
void sysfs_init(void) {
  if (sysfs_root)
    return;
  sysfs_root = node_alloc("", true);
  if (!sysfs_root) {
    printk(LOG_ERROR, "sysfs_init: failed to alloc root\n");
    return;
  }
  struct sysfs_node *dev = sysfs_create_dir(sysfs_root, "dev");
  if (!dev || !sysfs_create_dir(dev, "char") ||
      !sysfs_create_dir(dev, "block")) {
    printk(LOG_ERROR, "sysfs_init: failed to create /sys/dev topology\n");
    return;
  }
  printk(LOG_INFO, "sysfs_init: root node created\n");
}

#ifdef TEST
static ssize_t sysfs_selftest_show(char *buf, size_t len, void *priv) {
  if (len < 2 || !priv)
    return -EINVAL;
  buf[0] = *(int *)priv == 42 ? 'Y' : 'N';
  buf[1] = '\n';
  return 2;
}
#endif

void sysfs_lifecycle_selftest(void) {
#ifdef TEST
  struct sysfs_node *dir = sysfs_create_dir(sysfs_root, "__lifecycle_test");
  BUG_ON(!dir);
  int *priv = kmalloc(sizeof(*priv));
  struct sysfs_attr *attr = kmalloc(sizeof(*attr));
  BUG_ON(!priv || !attr);
  *priv = 42;
  *attr = (struct sysfs_attr){
      .name = "value",
      .priv = priv,
      .show = sysfs_selftest_show,
  };
  struct sysfs_node *file = sysfs_create_file(dir, "value", attr);
  BUG_ON(!file);
  file->attr_owned = true;
  file->attr_priv_owned = true;

  struct inode *ip = sysfs_lookup("__lifecycle_test/value");
  BUG_ON(!ip);
  sysfs_remove_dir(dir);
  BUG_ON(sysfs_lookup("__lifecycle_test/value") != NULL);

  struct sysfs_attr *open_attr = ip->i_priv;
  char value[2];
  BUG_ON(!open_attr ||
         open_attr->show(value, sizeof(value), open_attr->priv) != 2);
  BUG_ON(value[0] != 'Y');
  inode_put(ip);
  printk(LOG_INFO, "sysfs lifecycle selftest: PASS\n");
#endif
}

struct sysfs_node *sysfs_root_node(void) { return sysfs_root; }

struct devchar_uevent_priv {
  char devname[32];
  unsigned major;
  unsigned minor;
};

static ssize_t devchar_uevent_show(char *buf, size_t len, void *priv) {
  struct devchar_uevent_priv *p = (struct devchar_uevent_priv *)priv;
  if (!p)
    return -EIO;
  return snprintf(buf, len, "MAJOR=%u\nMINOR=%u\nDEVNAME=%s\n", p->major,
                  p->minor, p->devname);
}

void sysfs_devchar_unregister(unsigned major, unsigned minor) {
  char name[32];
  int n = snprintf(name, sizeof(name), "%u:%u", major, minor);
  if (n < 0 || (size_t)n >= sizeof(name))
    return;
  struct sysfs_node *node = sysfs_walk("dev/char");
  if (!node)
    return;
  spin_lock(&sysfs_lock);
  struct sysfs_node *found = NULL;
  for (struct sysfs_node *c = node->children; c; c = c->sibling) {
    if (__strcmp(c->name, name) == 0) {
      found = c;
      break;
    }
  }
  spin_unlock(&sysfs_lock);
  if (found)
    sysfs_remove_dir(found);
}

struct sysfs_node *sysfs_devchar_register(unsigned major, unsigned minor,
                                          const char *devname,
                                          const char *subsystem_target) {
  if (major > 4095 || minor > 1048575 || !devname || !subsystem_target ||
      __strlen(devname) >= sizeof(((struct devchar_uevent_priv *)0)->devname))
    return NULL;

  char name[32];
  int name_len = snprintf(name, sizeof(name), "%u:%u", major, minor);
  if (name_len < 0 || (size_t)name_len >= sizeof(name))
    return NULL;

  struct sysfs_node *char_dir = sysfs_walk("dev/char");
  if (!char_dir)
    return NULL;
  sysfs_devchar_unregister(major, minor);

  struct sysfs_node *root = sysfs_create_dir(char_dir, name);
  struct devchar_uevent_priv *priv = kmalloc(sizeof(*priv));
  struct sysfs_attr *attr = kmalloc(sizeof(*attr));
  if (!root || !priv || !attr)
    goto fail;
  __memset(priv, 0, sizeof(*priv));
  __strncpy(priv->devname, devname, sizeof(priv->devname) - 1);
  priv->major = major;
  priv->minor = minor;
  attr->name = "uevent";
  attr->priv = priv;
  attr->show = devchar_uevent_show;
  attr->store = NULL;
  struct sysfs_node *uevent = sysfs_create_file(root, "uevent", attr);
  if (!uevent)
    goto fail;
  uevent->attr_owned = true;
  uevent->attr_priv_owned = true;
  priv = NULL;
  attr = NULL;

  struct sysfs_node *device = sysfs_create_dir(root, "device");
  if (!device || !sysfs_create_dir(device, "drm") ||
      !sysfs_create_symlink(device, "subsystem", subsystem_target))
    goto fail;
  return root;

fail:
  kfree(priv);
  kfree(attr);
  if (root)
    sysfs_remove_dir(root);
  return NULL;
}

struct sysfs_node *sysfs_devchar_add_device_child(struct sysfs_node *root,
                                                  const char *group,
                                                  const char *name) {
  if (!root || !group || !name)
    return NULL;
  struct sysfs_node *device = NULL;
  for (struct sysfs_node *n = root->children; n; n = n->sibling) {
    if (n->is_dir && __strcmp(n->name, "device") == 0) {
      device = n;
      break;
    }
  }
  if (!device)
    return NULL;
  struct sysfs_node *group_dir = NULL;
  for (struct sysfs_node *n = device->children; n; n = n->sibling) {
    if (n->is_dir && __strcmp(n->name, group) == 0) {
      group_dir = n;
      break;
    }
  }
  return group_dir ? sysfs_create_dir(group_dir, name) : NULL;
}

struct sysfs_node *
sysfs_devchar_add_device_file(struct sysfs_node *root, const char *name,
                              const struct sysfs_attr *attr) {
  if (!root || !name || !attr)
    return NULL;
  for (struct sysfs_node *n = root->children; n; n = n->sibling) {
    if (n->is_dir && __strcmp(n->name, "device") == 0)
      return sysfs_create_file(n, name, attr);
  }
  return NULL;
}

// evdev show callback (priv = input_dev_props*)
static ssize_t evdev_show_name(char *buf, size_t len, void *priv) {
  struct input_dev_props *p = (struct input_dev_props *)priv;
  if (!p)
    return snprintf(buf, len, "\n");
  return snprintf(buf, len, "%s\n", p->name);
}
static ssize_t evdev_show_bustype(char *buf, size_t len, void *priv) {
  struct input_dev_props *p = (struct input_dev_props *)priv;
  if (!p)
    return snprintf(buf, len, "0\n");
  return snprintf(buf, len, "%u\n", p->bustype);
}
static ssize_t evdev_show_vendor(char *buf, size_t len, void *priv) {
  struct input_dev_props *p = (struct input_dev_props *)priv;
  if (!p)
    return snprintf(buf, len, "0x0000\n");
  return snprintf(buf, len, "0x%04X\n", p->vendor);
}
static ssize_t evdev_show_product(char *buf, size_t len, void *priv) {
  struct input_dev_props *p = (struct input_dev_props *)priv;
  if (!p)
    return snprintf(buf, len, "0x0000\n");
  return snprintf(buf, len, "0x%04X\n", p->product);
}
static ssize_t evdev_show_version(char *buf, size_t len, void *priv) {
  struct input_dev_props *p = (struct input_dev_props *)priv;
  if (!p)
    return snprintf(buf, len, "0x0000\n");
  return snprintf(buf, len, "0x%04X\n", p->version);
}

const struct sysfs_attr evdev_attr_name = {.name = "name",
                                           .show = evdev_show_name};
const struct sysfs_attr evdev_attr_bustype = {.name = "bustype",
                                              .show = evdev_show_bustype};
const struct sysfs_attr evdev_attr_vendor = {.name = "vendor",
                                             .show = evdev_show_vendor};
const struct sysfs_attr evdev_attr_product = {.name = "product",
                                              .show = evdev_show_product};
const struct sysfs_attr evdev_attr_version = {.name = "version",
                                              .show = evdev_show_version};

// Writable uevent attribute template (for coldplug): priv = uevent_attr_priv*.
// devtmpfs copies this template per-device and fills priv (mirroring the evdev
// id/attr copy pattern) so uevent_store stays static (no symbol export).
// show=NULL (this round is write-only; making Linux-style uevent readable is a
// todo).
const struct sysfs_attr uevent_attr = {
    .name = "uevent", .show = NULL, .store = uevent_store};

// ringbuf_fops (SHM ring consumer read/poll/mmap) removed — the evdev broker
// (kernel/bsd/evdev_broker.c) now owns per-fd kfifo consumer state directly,
// replacing the SHM output ring. See refact_evdev.md §5.

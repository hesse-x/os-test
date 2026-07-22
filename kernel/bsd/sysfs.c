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
#include "kernel/bsd/mount.h"
#include "kernel/bsd/netlink.h"
#include "kernel/bsd/types.h"
#include "kernel/xcore/kpi.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/spinlock.h"
#include <xos/errno.h>
#include <xos/fcntl.h>
#include <xos/stat.h>

/* ===== sysfs 节点树 ===== */
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
  /* 检查是否已存在 */
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

void sysfs_remove_dir(struct sysfs_node *dir) {
  if (!dir || !dir->parent)
    return;
  spin_lock(&sysfs_lock);
  /* 从父节点 children 链中摘除 */
  struct sysfs_node **pp = &dir->parent->children;
  while (*pp && *pp != dir)
    pp = &(*pp)->sibling;
  if (*pp)
    *pp = dir->sibling;
  /* 递归释放子节点 */
  struct sysfs_node *c = dir->children;
  while (c) {
    struct sysfs_node *next = c->sibling;
    if (c->ip)
      inode_put(c->ip);
    if (c->attr_owned)
      kfree(c->attr);
    kfree(c);
    c = next;
  }
  if (dir->ip)
    inode_put(dir->ip);
  if (dir->attr_owned)
    kfree(dir->attr);
  kfree(dir);
  spin_unlock(&sysfs_lock);
}

struct sysfs_node *sysfs_class_dir(const char *subsystem) {
  static struct sysfs_node *class_dir;
  if (!class_dir)
    class_dir = sysfs_create_dir(sysfs_root, "class");
  return sysfs_create_dir(class_dir, subsystem);
}

/* ===== 查找: 逐组件走树 ===== */
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

static struct inode *sysfs_node_to_inode(struct sysfs_node *n) {
  if (n->ip) {
    n->ip->i_op = n->is_dir ? &sysfs_dir_iop : &sysfs_file_iop;
    return inode_get(n->ip);
  }
  int type = n->is_dir ? INODE_DIR : INODE_REGULAR;
  struct inode *ip = inode_create(n->ino, type, 0, 0, 0, 0);
  if (!ip)
    return NULL;
  /* S08: sysfs 属性文件只读 0100444(inode_create 默认 0100644 可写,不符 sysfs
   * 语义);目录 0040755。owner 默认 0 root(内核建)。 */
  ip->mode = n->is_dir ? 0040755 : 0100444;
  ip->i_priv = n->is_dir ? (void *)n : (void *)n->attr;
  ip->i_op = n->is_dir ? &sysfs_dir_iop : &sysfs_file_iop;
  n->ip = inode_get(ip);
  return ip;
}

/* sysfs_dir_lookup:在目录 inode dir 内查名为 name 的子项,返 +1 inode 或 NULL。
 */
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

/* sysfs_getattr:从 ip 字段填(不 deref i_priv,避免 dir/node 与 file/attr
 * 脆弱判别)。S08: 报真实 ip->mode/uid/gid(sysfs inode 内核建,owner 默认 0)。 */
static int sysfs_getattr(struct inode *ip, struct kstat *ks) {
  __memset(ks, 0, sizeof(*ks));
  ks->st_ino = ip->ino;
  ks->st_mode = ip->mode;
  ks->st_uid = ip->uid;
  ks->st_gid = ip->gid;
  ks->st_nlink = 1;
  ks->st_size = 0;
  ks->st_blksize = 4096;
  return 0;
}

static const struct inode_operations sysfs_dir_iop = {
    .lookup = sysfs_dir_lookup,
    .getattr = sysfs_getattr,
};

static const struct inode_operations sysfs_file_iop = {
    .getattr = sysfs_getattr,
};

/* sysfs_mount_root:返回 /sys 根 inode(已 inode_get)。 */
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
  struct sysfs_node *c = n->children;
  while (c) {
    size_t nl = __strlen(c->name);
    unsigned dt = c->is_dir ? DT_DIR : DT_REG;
    uint16_t r = (uint16_t)((sizeof(struct dirent64) + nl + 1 + 7) & ~7);
    if (cur_pos < ctx->pos) {
      cur_pos += r;
      c = c->sibling;
      continue;
    }
    if (!dir_emit(ctx, c->name, (int)nl, cur_pos, c->ino, dt))
      break;
    cur_pos += r;
    c = c->sibling;
  }
  ctx->pos = (uint64_t)-1;
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
  ks->st_mode = n->is_dir ? 0040755 : 0100444;
  ks->st_ino = n->ino;
  ks->st_nlink = 1;
  ks->st_size = 0;
  ks->st_blksize = 512;
  return 0;
}

/* ===== uevent store 回调(对齐 Linux kobject uevent 属性写) ===== */
/* uevent_store:写 "add" 到 /sys/.../uevent → 重广播 uevent。
 * 走与原始广播(devtmpfs.c device_create/device_set_meta)完全相同的 netlink
 * 路径(nl_uevent_broadcast),使 coldplug 与热插拔同路。本轮只接受 "add"
 * action(Linux 接受 add/remove/change,记 todo)。 */
static ssize_t uevent_store(const char *buf, size_t len, void *priv) {
  struct uevent_attr_priv *p = (struct uevent_attr_priv *)priv;
  if (!p)
    return -EIO;
  /* 解析 action:到首个 '\n'/'\0' 止 */
  size_t alen = 0;
  while (alen < len && buf[alen] != '\n' && buf[alen] != '\0')
    alen++;
  if (alen == 3 && __memcmp(buf, "add", 3) == 0) {
    nl_uevent_broadcast("add", p->devpath, p->subsystem);
    return (ssize_t)len;
  }
  /* 本轮只 input coldplug 用 add;remove/change 记 todo */
  return -EINVAL;
}

/* ===== sysfs_fops.write ===== */
static ssize_t sysfs_file_write(struct xtask *proc, struct file *f,
                                const void *buf, size_t count) {
  (void)proc;
  struct inode *ip = f->inode;
  if (!ip || !ip->i_priv)
    return -ENODEV;
  /* sysfs_fops 同时服务 read/write:防 O_RDONLY 写 */
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

/* ===== sysfs_fops.read ===== */
static ssize_t sysfs_file_read(struct xtask *proc, struct file *f, void *buf,
                               size_t count) {
  (void)proc;
  struct inode *ip = f->inode;
  if (!ip || !ip->i_priv)
    return -ENODEV;
  struct sysfs_attr *attr = (struct sysfs_attr *)ip->i_priv;
  if (!attr->show)
    return 0;
  if (count > 4096)
    count = 4096;
  char kbuf[4096];
  ssize_t n = attr->show(kbuf, count, attr->priv);
  if (n < 0)
    return n;
  if (n > (ssize_t)count)
    n = (ssize_t)count;
  if (copy_to_user(buf, kbuf, (size_t)n))
    return -EFAULT;
  return n;
}

const struct file_operations sysfs_fops = {
    .read = sysfs_file_read,
    .write = sysfs_file_write,
};

/* ===== fstype ===== */
/* R1 stub:返 NULL。R3(plan_vfs1.md)以 sysfs_mount_root 取代。 */
struct fstype sysfs_fstype = {
    .name = "sysfs",
    .mount_root = sysfs_mount_root,
    .getdents = sysfs_getdents,
};

/* ===== 初始化 ===== */
void sysfs_init(void) {
  if (sysfs_root)
    return;
  sysfs_root = node_alloc("", true);
  if (!sysfs_root) {
    printk(LOG_ERROR, "sysfs_init: failed to alloc root\n");
    return;
  }
  printk(LOG_INFO, "sysfs_init: root node created\n");
}

struct sysfs_node *sysfs_root_node(void) { return sysfs_root; }

/* evdev show 回调 (priv = input_dev_props*) */
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

/* 可写 uevent 属性模板(coldplug 用):priv = uevent_attr_priv*。devtmpfs 逐设备
 * 拷贝此模板并填 priv(对齐 evdev id/ attr 拷贝模式),使 uevent_store 保持
 * static(无需导出符号)。show=NULL(本轮只写不读,Linux uevent 可读记 todo)。 */
const struct sysfs_attr uevent_attr = {
    .name = "uevent", .show = NULL, .store = uevent_store};

/* ringbuf_fops (SHM ring consumer read/poll/mmap) removed — the evdev broker
 * (kernel/bsd/evdev_broker.c) now owns per-fd kfifo consumer state directly,
 * replacing the SHM output ring. See refact_evdev.md §5. */

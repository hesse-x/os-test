/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#include "kernel/bsd/sysfs.h"

#include "arch/x64/utils.h"
#include "kernel/bsd/devtmpfs.h"
#include "kernel/bsd/fops.h"
#include "kernel/bsd/inode.h"
#include "kernel/bsd/mount.h"
#include "kernel/bsd/ring.h"
#include "kernel/bsd/types.h"
#include "kernel/xcore/kpi.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/xtask.h"
#include "xos/syscall_nums.h"
#include <xos/errno.h>
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
 * 脆弱判别)。 */
static int sysfs_getattr(struct inode *ip, struct kstat *ks) {
  __memset(ks, 0, sizeof(*ks));
  ks->st_ino = ip->ino;
  ks->st_mode = (ip->type == INODE_DIR) ? 0040755 : 0100644;
  ks->st_nlink = 1;
  ks->st_size = 0;
  ks->st_blksize = 512;
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
  if (ctx->pos != 0) {
    spin_unlock(&sysfs_lock);
    return 0;
  }
  struct sysfs_node *c = n->children;
  while (c) {
    size_t nl = __strlen(c->name);
    unsigned dt = c->is_dir ? DT_DIR : DT_REG;
    if (!dir_emit(ctx, c->name, (int)nl, ctx->written, c->ino, dt))
      break;
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

/* ===== ringbuf_fops: SHM ring buffer 事件流 (design 3.5) ===== */
#include <xos/input.h>

static ssize_t ringbuf_read(struct xtask *proc, struct file *f, void *buf,
                            size_t count) {
  (void)proc;
  if (count == 0)
    return 0;
  struct inode *ip = f->inode;
  if (!ip || !ip->shm)
    return -ENODEV;
  ring_t r = ring_from_shm(ip->shm);
  return ring_read(&r, f, buf, count);
}

static __poll ringbuf_poll(struct xtask *proc, struct file *f, int events) {
  (void)proc;
  struct inode *ip = f->inode;
  if (!ip || !ip->shm)
    return 0;
  ring_t r = ring_from_shm(ip->shm);
  return ring_poll(&r, f, events);
}

static int ringbuf_close(struct xtask *proc, struct file *f) {
  struct inode *ip = f->inode;
  if (!ip || !ip->i_priv)
    return 0;
  struct dev_ops *ops = (struct dev_ops *)ip->i_priv;
  if (ops->driver_pid > 0) {
    /* 发 RINGBUF_CLOSE 通知给 driver (design 3.6) */
    recv_msg msg;
    __memset(&msg, 0, sizeof(msg));
    msg.type = RECV_NOTIFY;
    msg.src = (uint32_t)proc->pid;
    struct ringbuf_lifecycle_msg lm = {.opcode = RINGBUF_CLOSE,
                                       .pid = proc->pid};
    __strncpy(lm.name, "", 31);
    __memcpy(msg.data, &lm, sizeof(lm));
    notify_and_wake(ops->driver_pid, &msg);
  }
  return 0;
}

void ringbuf_init_cursor(struct inode *ip, struct file *f) {
  if (!ip || !ip->shm)
    return;
  ring_t r = ring_from_shm(ip->shm);
  if (r.hdr)
    f->offset = r.hdr->head;
}

void ringbuf_notify_open(struct inode *ip, int32_t opener_pid) {
  if (!ip || !ip->i_priv)
    return;
  struct dev_ops *ops = (struct dev_ops *)ip->i_priv;
  if (ops->driver_pid <= 0)
    return;
  recv_msg msg;
  __memset(&msg, 0, sizeof(msg));
  msg.type = RECV_NOTIFY;
  msg.src = (uint32_t)opener_pid;
  struct ringbuf_lifecycle_msg lm = {.opcode = RINGBUF_OPEN, .pid = opener_pid};
  __strncpy(lm.name, "", 31);
  __memcpy(msg.data, &lm, sizeof(lm));
  notify_and_wake(ops->driver_pid, &msg);
}

static uint64_t ringbuf_mmap(struct xtask *proc, struct file *f,
                             uint64_t size) {
  (void)size;
  struct inode *ip = f->inode;
  if (!ip || !ip->i_priv)
    return -EPERM;
  struct dev_ops *ops = (struct dev_ops *)ip->i_priv;
  /* 仅 driver_pid 允许 mmap (防止消费者绕过内核直接 mmap) */
  if (ops->driver_pid != proc->pid)
    return -EPERM;
  if (!ip->shm)
    return -ENODEV;
  /* Driver owner: return -ENOSYS so sys_mmap falls through to the FD_DEV
   * SHM page-mapping path. Consumers are rejected by -EPERM above
   * (sys_mmap does not fall through on EPERM). */
  return -ENOSYS;
}

const struct file_operations ringbuf_fops = {
    .read = ringbuf_read,
    .poll = ringbuf_poll,
    .close = ringbuf_close,
    .mmap = ringbuf_mmap,
};

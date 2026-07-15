/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * tmpfs: 内存文件系统，承载 /run（udevd db 前置）。重启清空，进程 crash
 * 不影响。 ino 从 0xC0000000 段自维护分配，避开 devtmpfs 0x80000000 + FAT32
 * cluster 段。 注：INODE_DIR 经 inode_create 强制走 next_dev_ino（dev
 * 段），仅文件/socket 落 0xC0000000+ 段；各段由全局 inode hash
 * 去重，无需段间协调。
 */

#include "kernel/bsd/tmpfs.h"

#include <stddef.h>
#include <stdint.h>

#include "arch/x64/utils.h"
#include "kernel/bsd/fops.h"
#include "kernel/bsd/inode.h"
#include "kernel/bsd/mount.h"
#include "kernel/bsd/types.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/kpi.h"
#include "kernel/xcore/mem/kasan.h"
#include "kernel/xcore/spinlock.h"

#include <xos/errno.h>
#include <xos/stat.h>

struct xtask;

/* ===== 私有 inode 数据，挂 inode->i_priv =====
 * 普通文件/socket: data 存内容；目录: children 链表头。
 * socket inode 的 i_priv 由 socket 层 bind 时改挂 unix_sock*（见 socket.c）。
 */
struct tmpfs_inode_info {
  struct inode *inode; /* 回指 */
  struct tmpfs_inode_info *parent;
  struct tmpfs_inode_info *children; /* 头插 */
  struct tmpfs_inode_info *sibling;
  char name[256]; /* 目录项名（根节点为空） */
  void *data;     /* 普通文件内容 */
  size_t size;
  size_t cap;
  spinlock lock; /* 保护 data/size/children */
};

static struct inode *tmpfs_root_inode;
static uint32_t next_tmpfs_ino = 0xC0000000;
static spinlock tmpfs_ino_lock = SPINLOCK_INIT;

/* 容量上限 */
#define TMPFS_FILE_CAP (64 * 1024)        /* 单文件 64KB */
#define TMPFS_TOTAL_CAP (1 * 1024 * 1024) /* 总量 1MB */
static size_t tmpfs_total_used = 0;
static spinlock tmpfs_total_lock = SPINLOCK_INIT;

static uint32_t tmpfs_alloc_ino(void) {
  spin_lock(&tmpfs_ino_lock);
  uint32_t ino = next_tmpfs_ino++;
  spin_unlock(&tmpfs_ino_lock);
  return ino;
}

/* 建一个 tmpfs_inode_info 并挂到新 inode->i_priv */
static struct tmpfs_inode_info *
new_tmpfs_info(struct inode *inode, struct tmpfs_inode_info *parent) {
  struct tmpfs_inode_info *ti =
      (struct tmpfs_inode_info *)kmalloc(sizeof(struct tmpfs_inode_info));
  if (!ti)
    return NULL;
  ti->inode = inode;
  ti->parent = parent;
  ti->children = NULL;
  ti->sibling = NULL;
  ti->name[0] = '\0';
  ti->data = NULL;
  ti->size = 0;
  ti->cap = 0;
  ti->lock = SPINLOCK_INIT;
  return ti;
}

/* ===== i_op 回调 ===== */
static struct inode *tmpfs_lookup(struct inode *dir, const char *name);
static struct inode *tmpfs_create(struct inode *dir, const char *name,
                                  int mode);
static int tmpfs_mkdir(struct inode *dir, const char *name, int mode);
static int tmpfs_unlink(struct inode *dir, const char *name);
static int tmpfs_rmdir(struct inode *dir, const char *name);
static int tmpfs_getattr(struct inode *ip, struct kstat *ks);
static int tmpfs_setattr(struct inode *ip, uint64_t size);

static const struct inode_operations tmpfs_dir_iop = {
    .lookup = tmpfs_lookup,
    .create = tmpfs_create,
    .mkdir = tmpfs_mkdir,
    .unlink = tmpfs_unlink,
    .rmdir = tmpfs_rmdir,
    .getattr = tmpfs_getattr,
    .setattr = tmpfs_setattr,
};

static const struct inode_operations tmpfs_file_iop = {
    .getattr = tmpfs_getattr,
    .setattr = tmpfs_setattr,
};

/* ===== i_op 实现 ===== */
static int tmpfs_getattr(struct inode *ip, struct kstat *ks) {
  if (!ip || !ks)
    return -EFAULT;
  __memset(ks, 0, sizeof(*ks));
  ks->st_ino = ip->ino;
  ks->st_mode = ip->mode;
  ks->st_nlink = (uint64_t)ip->nlink;
  ks->st_size = (int64_t)ip->size;
  ks->st_blksize = 4096;
  /* st_uid/st_gid/st_rdev/st_*tim 留 0（对齐 fat32_getattr 现状，本 OS 无
   * uid/gid/timestamp 语义） */
  return 0;
}

static int tmpfs_setattr(struct inode *ip, uint64_t size) {
  struct tmpfs_inode_info *ti = (struct tmpfs_inode_info *)ip->i_priv;
  if (!ti)
    return -EFAULT;
  spin_lock(&ti->lock);
  if (size == 0) {
    if (ti->data) {
      spin_lock(&tmpfs_total_lock);
      tmpfs_total_used -= ti->cap;
      spin_unlock(&tmpfs_total_lock);
      kfree(ti->data);
      ti->data = NULL;
    }
    ti->size = 0;
    ti->cap = 0;
  } else if (size > ti->cap) {
    if (size > TMPFS_FILE_CAP) {
      spin_unlock(&ti->lock);
      return -ENOSPC;
    }
    spin_lock(&tmpfs_total_lock);
    if (tmpfs_total_used + (size - ti->cap) > TMPFS_TOTAL_CAP) {
      spin_unlock(&tmpfs_total_lock);
      spin_unlock(&ti->lock);
      return -ENOSPC;
    }
    tmpfs_total_used += (size - ti->cap);
    spin_unlock(&tmpfs_total_lock);
    void *nd = krealloc(ti->data, size);
    if (!nd) {
      /* 回滚总量记账 */
      spin_lock(&tmpfs_total_lock);
      tmpfs_total_used -= (size - ti->cap);
      spin_unlock(&tmpfs_total_lock);
      spin_unlock(&ti->lock);
      return -ENOMEM;
    }
    ti->data = nd;
    ti->cap = size;
  }
  ti->size = size;
  ip->size = size;
  spin_unlock(&ti->lock);
  return 0;
}

static struct inode *tmpfs_lookup(struct inode *dir, const char *name) {
  struct tmpfs_inode_info *ti = (struct tmpfs_inode_info *)dir->i_priv;
  if (!ti)
    return NULL;
  spin_lock(&ti->lock);
  for (struct tmpfs_inode_info *c = ti->children; c; c = c->sibling) {
    if (__strcmp(c->name, name) == 0) {
      inode_get(c->inode);
      spin_unlock(&ti->lock);
      return c->inode;
    }
  }
  spin_unlock(&ti->lock);
  return NULL;
}

/* 内部建节点：按 type 分配 ino + 挂 i_op + 建并挂 tmpfs_inode_info 到父
 * children。 create 路径把调用者传入的 mode（含 S_IFSOCK/S_IFIFO 类型位 +
 * 权限位）写 ip->mode， 覆盖 inode_create 默认 mode，使 stat 原样返回（对齐
 * Linux mknod）。 */
static struct inode *tmpfs_new_node(struct tmpfs_inode_info *parent_ti,
                                    const char *name, int type, int mode,
                                    int keep_mode) {
  uint32_t ino = tmpfs_alloc_ino();
  struct inode *ip = inode_create(ino, type, 0, 0, 0, 0);
  if (!ip)
    return NULL;
  if (keep_mode)
    ip->mode = (uint32_t)mode;
  ip->i_op = (type == INODE_DIR) ? &tmpfs_dir_iop : &tmpfs_file_iop;
  struct tmpfs_inode_info *ti = new_tmpfs_info(ip, parent_ti);
  if (!ti) {
    inode_put(ip);
    return NULL;
  }
  if (name) {
    int i = 0;
    while (name[i] && i < 255) {
      ti->name[i] = name[i];
      i++;
    }
    ti->name[i] = '\0';
  }
  ip->i_priv = ti;
  /* 挂父 children 链（头插） */
  if (parent_ti) {
    ti->sibling = parent_ti->children;
    parent_ti->children = ti;
    /* 目录项持有一个 inode 引用(对齐 Linux dentry→inode):inode_create 出口
     * i_count=1 作 create/mkdir 返回的 +1 给调用者,此处再 +1 留给目录项自身,
     * 使文件存在期间 inode 不被调用者 put 释放。否则 sys_mknod/open 等调用者
     * put 掉返回的 +1 后 i_count→0 触发 inode kfree,留下 children 链里的悬空
     * 指针,后续 lookup→inode_get 命中 i_count==0 的 ASSERT 而 PANIC。
     * fat32 无此问题:其目录项在磁盘簇,lookup 经 inode_get_or_create 重取活
     * inode。 */
    inode_get(ip);
  }
  return ip;
}

static struct inode *tmpfs_create(struct inode *dir, const char *name,
                                  int mode) {
  struct tmpfs_inode_info *parent_ti = (struct tmpfs_inode_info *)dir->i_priv;
  if (!parent_ti)
    return ERR_PTR(-EFAULT);
  /* 重名检查：tmpfs_lookup 返 +1 引用，须 put 平衡 */
  struct inode *exist = tmpfs_lookup(dir, name);
  if (exist) {
    inode_put(exist);
    return ERR_PTR(-EEXIST);
  }
  /* mode & S_IFMT：S_IFSOCK 建 INODE_SOCKET；S_IFIFO 暂以普通文件承载
   * （无 pipe fs），mode 保留 S_IFIFO 位供 stat 区分；其余建 INODE_REGULAR。 */
  int type = INODE_REGULAR;
  if ((mode & S_IFMT) == S_IFSOCK)
    type = INODE_SOCKET;
  struct inode *ip = tmpfs_new_node(parent_ti, name, type, mode, 1);
  if (!ip)
    return ERR_PTR(-ENOMEM);
  return ip; /* i_count=2：1 目录项 + 1 返回给调用者(由调用者 put) */
}

static int tmpfs_mkdir(struct inode *dir, const char *name, int mode) {
  (void)mode;
  struct tmpfs_inode_info *parent_ti = (struct tmpfs_inode_info *)dir->i_priv;
  if (!parent_ti)
    return -EFAULT;
  /* 重名检查：tmpfs_lookup 返 +1 引用，须 put 平衡 */
  struct inode *exist = tmpfs_lookup(dir, name);
  if (exist) {
    inode_put(exist);
    return -EEXIST;
  }
  struct inode *ip = tmpfs_new_node(parent_ti, name, INODE_DIR, 0, 0);
  if (!ip)
    return -ENOMEM;
  inode_put(ip); /* mkdir 不返 inode，平衡 tmpfs_new_node 出口的 +1 返回引用 */
  return 0;
}

static int tmpfs_unlink(struct inode *dir, const char *name) {
  struct tmpfs_inode_info *parent_ti = (struct tmpfs_inode_info *)dir->i_priv;
  if (!parent_ti)
    return -EFAULT;
  spin_lock(&parent_ti->lock);
  struct tmpfs_inode_info *prev = NULL, *c = parent_ti->children;
  while (c) {
    if (__strcmp(c->name, name) == 0) {
      if (prev)
        prev->sibling = c->sibling;
      else
        parent_ti->children = c->sibling;
      /* 摘目录项：仅当 inode 无其它引用（i_count==1）时立即回收
       * tmpfs_inode_info， 否则保留（unlink-while-open / bind
       * 持引用场景），引用计数管释放。 注意 socket inode 的 i_priv 此时可能挂
       * unix_sock*，不在此回收（socket 层 unix_bind_unregister 负责）。 */
      struct inode *ip = c->inode;
      int last = (refcount_read(&ip->i_count) == 1);
      int is_dir = (ip->type == INODE_DIR);
      spin_unlock(&parent_ti->lock);
      inode_put(ip); /* 摘目录项的引用；若 i_count 归 0 触发 inode kfree */
      if (last && !is_dir) {
        /* 普通文件且无打开 fd：回收 data（socket inode 的 i_priv 归 socket 层）
         */
        if (c->data) {
          spin_lock(&tmpfs_total_lock);
          tmpfs_total_used -= c->cap;
          spin_unlock(&tmpfs_total_lock);
          kfree(c->data);
        }
        kfree(c);
      }
      return 0;
    }
    prev = c;
    c = c->sibling;
  }
  spin_unlock(&parent_ti->lock);
  return -ENOENT;
}

static int tmpfs_rmdir(struct inode *dir, const char *name) {
  struct tmpfs_inode_info *parent_ti = (struct tmpfs_inode_info *)dir->i_priv;
  if (!parent_ti)
    return -EFAULT;
  spin_lock(&parent_ti->lock);
  struct tmpfs_inode_info *prev = NULL, *c = parent_ti->children;
  while (c) {
    if (__strcmp(c->name, name) == 0 && c->inode &&
        c->inode->type == INODE_DIR) {
      if (c->children) {
        spin_unlock(&parent_ti->lock);
        return -ENOTEMPTY;
      }
      if (prev)
        prev->sibling = c->sibling;
      else
        parent_ti->children = c->sibling;
      struct inode *ip = c->inode;
      spin_unlock(&parent_ti->lock);
      kfree(c);
      inode_put(ip);
      return 0;
    }
    prev = c;
    c = c->sibling;
  }
  spin_unlock(&parent_ti->lock);
  return -ENOENT;
}

/* ===== getdents（fstype 回调）===== */
static ssize_t tmpfs_getdents(struct inode *dir, struct dir_context *ctx) {
  struct tmpfs_inode_info *ti = (struct tmpfs_inode_info *)dir->i_priv;
  if (!ti)
    return 0;
  spin_lock(&ti->lock);
  if (ctx->pos != 0) {
    spin_unlock(&ti->lock);
    return 0;
  }
  struct tmpfs_inode_info *c = ti->children;
  while (c) {
    size_t nl = __strlen(c->name);
    unsigned dt = (c->inode && c->inode->type == INODE_DIR) ? DT_DIR : DT_REG;
    if (c->inode && c->inode->type == INODE_SOCKET)
      dt = DT_SOCK;
    if (!dir_emit(ctx, c->name, (int)nl, ctx->written, c->inode->ino, dt))
      break;
    c = c->sibling;
  }
  ctx->pos = (uint64_t)-1;
  spin_unlock(&ti->lock);
  return (ssize_t)ctx->written;
}

/* ===== mount_root ===== */
static struct inode *tmpfs_mount_root(struct mount_entry *m) {
  (void)m;
  if (!tmpfs_root_inode) {
    uint32_t ino = tmpfs_alloc_ino();
    tmpfs_root_inode = inode_create(ino, INODE_DIR, 0, 0, 0, 0);
    if (!tmpfs_root_inode)
      return NULL;
    tmpfs_root_inode->i_op = &tmpfs_dir_iop;
    tmpfs_root_inode->i_priv = new_tmpfs_info(tmpfs_root_inode, NULL);
  }
  return inode_get(tmpfs_root_inode);
}

/* ===== fops：read/write ===== */
static ssize_t tmpfs_read(struct xtask *proc, struct file *f, void *buf,
                          size_t count) {
  (void)proc;
  struct inode *ip = f->inode;
  struct tmpfs_inode_info *ti = (struct tmpfs_inode_info *)ip->i_priv;
  if (!ti)
    return -EFAULT;
  spin_lock(&ti->lock);
  size_t off = (size_t)f->offset;
  if (off >= ti->size) {
    spin_unlock(&ti->lock);
    return 0;
  }
  size_t n = ti->size - off < count ? ti->size - off : count;
  if (copy_to_user(buf, (char *)ti->data + off, n) != 0) {
    spin_unlock(&ti->lock);
    return -EFAULT;
  }
  f->offset = off + n;
  spin_unlock(&ti->lock);
  return (ssize_t)n;
}

static ssize_t tmpfs_write(struct xtask *proc, struct file *f, const void *buf,
                           size_t count) {
  (void)proc;
  struct inode *ip = f->inode;
  struct tmpfs_inode_info *ti = (struct tmpfs_inode_info *)ip->i_priv;
  if (!ti)
    return -EFAULT;
  size_t off = (size_t)f->offset;
  size_t need = off + count;
  if (need > TMPFS_FILE_CAP)
    return -ENOSPC;
  spin_lock(&ti->lock);
  if (need > ti->cap) {
    spin_lock(&tmpfs_total_lock);
    size_t delta = need - ti->cap;
    if (tmpfs_total_used + delta > TMPFS_TOTAL_CAP) {
      spin_unlock(&tmpfs_total_lock);
      spin_unlock(&ti->lock);
      return -ENOSPC;
    }
    tmpfs_total_used += delta;
    spin_unlock(&tmpfs_total_lock);
    void *nd = krealloc(ti->data, need);
    if (!nd) {
      spin_lock(&tmpfs_total_lock);
      tmpfs_total_used -= (need - ti->cap);
      spin_unlock(&tmpfs_total_lock);
      spin_unlock(&ti->lock);
      return -ENOMEM;
    }
    ti->data = nd;
    ti->cap = need;
  }
  if (copy_from_user((char *)ti->data + off, buf, count) != 0) {
    spin_unlock(&ti->lock);
    return -EFAULT;
  }
  if (need > ti->size)
    ti->size = need;
  ip->size = ti->size;
  f->offset = off + count;
  spin_unlock(&ti->lock);
  return (ssize_t)count;
}

const struct file_operations tmpfs_file_fops = {
    .read = tmpfs_read,
    .write = tmpfs_write,
};

/* ===== fstype ===== */
struct fstype tmpfs_fstype = {
    .name = "tmpfs",
    .mount_root = tmpfs_mount_root,
    .getdents = tmpfs_getdents,
};

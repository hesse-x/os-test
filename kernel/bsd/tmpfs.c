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

#include <stdbool.h>
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

#include <xos/dirent.h>
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

/* tmpfs 全局序列化锁(等价 Linux s_vfs_rename_mutex,序列化所有 tmpfs rename
 * 防跨目录死锁)。本 OS 无 semaphore/mutex,用 spinlock_t + irqsave。db 场景只
 * 同目录,但接口对齐 Linux 须支持跨目录。 */
static spinlock tmpfs_rename_lock = SPINLOCK_INIT;

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
static int tmpfs_rename(struct inode *old_dir, const char *old_name,
                        struct inode *new_dir, const char *new_name);
static int tmpfs_getattr(struct inode *ip, struct kstat *ks);
static int tmpfs_setattr(struct inode *ip, uint64_t size);

static const struct inode_operations tmpfs_dir_iop = {
    .lookup = tmpfs_lookup,
    .create = tmpfs_create,
    .mkdir = tmpfs_mkdir,
    .unlink = tmpfs_unlink,
    .rmdir = tmpfs_rmdir,
    .rename = tmpfs_rename, /* 本方案新增 */
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
  ks->st_uid = ip->uid;
  ks->st_gid = ip->gid;
  ks->st_nlink = (uint64_t)ip->nlink;
  ks->st_size = (int64_t)ip->size;
  ks->st_blksize = 4096;
  ks->st_blocks = (ip->size + 4095) / 4096;
  /* S08: st_uid/st_gid 现报真实 ip->uid/gid(创建时由 sys_open/mkdir/mknod 设)。
   * st_rdev/st_*tim 仍留 0(本 OS 无设备号语义/时间戳,记 todo)。 */
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
   * （无 pipe fs），mode 保留 S_IFIFO 位供 stat 区分；其余建 INODE_REGULAR。
   * S08: open(O_CREAT) 传入的 mode 仅权限位(无类型位),tmpfs_new_node 以
   * keep_mode=1 原样写 ip->mode 会丢 S_IFREG 导致 stat S_ISREG 失败;此处对
   * 普通文件补 S_IFREG 类型位(socket 由 mknod 传入 S_IFSOCK,保留)。 */
  int type = INODE_REGULAR;
  if ((mode & S_IFMT) == S_IFSOCK)
    type = INODE_SOCKET;
  if (type == INODE_REGULAR && (mode & S_IFMT) == 0)
    mode = S_IFREG | (mode & 0777);
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

/* tmpfs_rename:完整 rename(2) 语义(对齐 Linux)。db 原子写基座(§3.1)。
 * 原子性靠 spinlock 互斥(不需要 dentry cache):同目录持 ti->lock 单段临界区;
 * 跨目录全局 tmpfs_rename_lock 序列化 + 按 inode 地址排序获取两把 ti->lock。
 * 回收纪律对齐 tmpfs_unlink:295-310:inode_put/kfree 出 ti->lock 临界区外执行。
 */
static int tmpfs_rename(struct inode *old_dir, const char *old_name,
                        struct inode *new_dir, const char *new_name) {
  if (!old_dir || !old_name || !new_dir || !new_name)
    return -EFAULT;

  /* old == new:no-op 对齐 Linux。显式处理避免"摘旧 new(=old 自己)→
   * 改名"逻辑误删自身。 */
  if (old_dir == new_dir && __strcmp(old_name, new_name) == 0)
    return 0;

  struct tmpfs_inode_info *old_ti = (struct tmpfs_inode_info *)old_dir->i_priv;
  struct tmpfs_inode_info *new_ti = (struct tmpfs_inode_info *)new_dir->i_priv;
  if (!old_ti || !new_ti)
    return -EFAULT;

  /* 全局序列化锁(跨目录死锁规避 + 原子性)。db 场景同目录单锁即可,但
   * 接口对齐 Linux rename(2) 须支持跨目录原子。irqsave 防 slab 分配的中断
   * 与本锁嵌套。 */
  uint64_t rflags;
  spin_lock_irqsave(&tmpfs_rename_lock, &rflags);

  /* 按 inode 地址排序获取 old_dir/new_dir 两把 ti->lock(防 a→b 与
   * b→a 锁序相反死锁;对齐 Linux vfs_rename 锁序协议)。 */
  struct tmpfs_inode_info *first_ti = (old_ti < new_ti) ? old_ti : new_ti;
  struct tmpfs_inode_info *second_ti = (old_ti < new_ti) ? new_ti : old_ti;
  spin_lock(&first_ti->lock);
  if (first_ti != second_ti)
    spin_lock(&second_ti->lock);

  /* 待回收信息(出锁后处理,对齐 tmpfs_unlink 回收纪律) */
  struct inode *reclaim_ip = NULL;
  struct tmpfs_inode_info *reclaim_node = NULL;
  int reclaim_last = 0, reclaim_is_dir = 0;

  /* 1. 从 old_dir 摘 old_name 节点 */
  struct tmpfs_inode_info *prev = NULL, *node = NULL;
  for (node = old_ti->children; node; prev = node, node = node->sibling) {
    if (__strcmp(node->name, old_name) == 0)
      break;
  }
  if (!node) {
    spin_unlock(&second_ti->lock);
    if (first_ti != second_ti)
      spin_unlock(&first_ti->lock);
    spin_unlock_irqrestore(&tmpfs_rename_lock, rflags);
    return -ENOENT;
  }
  if (prev)
    prev->sibling = node->sibling;
  else
    old_ti->children = node->sibling;

  /* 目录边界检查(对齐 Linux rename(2)):
   * - old 是目录、new 存在且非空 → -ENOTEMPTY
   * - old 是目录、new 存在且非目录 → -EISDIR
   * - old 非目录、new 存在且是目录 → -ENOTDIR
   * - old 是 new 祖先/new 是 old 祖先(循环) → -EINVAL
   * db 场景 old/new 均普通文件,不触发这些分支。 */
  int is_dir = (node->inode->type == INODE_DIR);
  struct tmpfs_inode_info *exist = NULL;
  for (exist = new_ti->children; exist; exist = exist->sibling) {
    if (__strcmp(exist->name, new_name) == 0)
      break;
  }
  if (exist) {
    int exist_is_dir = (exist->inode->type == INODE_DIR);
    int rc = 0;
    if (is_dir) {
      if (!exist_is_dir)
        rc = -EISDIR;
      else if (exist->children)
        rc = -ENOTEMPTY; /* new 目录非空 */
      /* 循环检测:沿 new 的 parent 链确认 old 不在其中(tmpfs_inode_info
       * .parent 回指,O(深度))。db 场景不触发。 */
      for (struct tmpfs_inode_info *a = new_ti; a; a = a->parent) {
        if (a == node) {
          rc = -EINVAL;
          break;
        }
      }
    } else {
      if (exist_is_dir)
        rc = -ENOTDIR;
    }
    if (rc) {
      /* 回滚:重新插回 old_dir */
      node->sibling = old_ti->children;
      old_ti->children = node;
      spin_unlock(&second_ti->lock);
      if (first_ti != second_ti)
        spin_unlock(&first_ti->lock);
      spin_unlock_irqrestore(&tmpfs_rename_lock, rflags);
      return rc;
    }
    /* 覆盖语义:从 new_ti 摘 exist 节点,目录项提交;inode/data 延后出锁回收
     * (对齐 tmpfs_unlink:287-302;已 open fd 靠 i_count>1 保 inode 不回收)。 */
    reclaim_ip = exist->inode;
    reclaim_node = exist;
    reclaim_last = (refcount_read(&reclaim_ip->i_count) == 1);
    reclaim_is_dir = exist_is_dir;
    struct tmpfs_inode_info *eprev = NULL, *e = new_ti->children;
    while (e) {
      if (e == exist) {
        if (eprev)
          eprev->sibling = e->sibling;
        else
          new_ti->children = e->sibling;
        break;
      }
      eprev = e;
      e = e->sibling;
    }
  }

  /* 2. 改名 + 头插 new_dir children 链 */
  int i = 0;
  while (new_name[i] && i < 255) {
    node->name[i] = new_name[i];
    i++;
  }
  node->name[i] = '\0';
  node->parent = new_ti;
  node->sibling = new_ti->children;
  new_ti->children = node;

  /* 3. 释放所有锁——目录项已提交,被覆盖 inode 不再经任何目录可达 */
  spin_unlock(&second_ti->lock);
  if (first_ti != second_ti)
    spin_unlock(&first_ti->lock);
  spin_unlock_irqrestore(&tmpfs_rename_lock, rflags);

  /* 4. 出锁后回收被覆盖节点(对齐 tmpfs_unlink:295-310 回收纪律;inode_put 取
   * inode_hash_lock + page_cache_invalidate + kfree,不能在 ti->lock 下)。 */
  if (reclaim_ip) {
    inode_put(reclaim_ip);
    if (reclaim_last && !reclaim_is_dir) {
      if (reclaim_node->data) {
        spin_lock(&tmpfs_total_lock);
        tmpfs_total_used -= reclaim_node->cap;
        spin_unlock(&tmpfs_total_lock);
        kfree(reclaim_node->data);
      }
      kfree(reclaim_node);
    }
  }
  return 0;
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

/* Emit the synthetic "." and ".." entries at the head of a directory listing.
 * Like a real child, each honors the resume cursor (skip while cur_pos <
 * ctx->pos) and stops on a buffer-full dir_emit. "." is the directory itself;
 * ".." is the parent (the tmpfs root points to itself, matching Linux). Returns
 * false if the buffer filled before the requested entry could be emitted. */
static bool tmpfs_emit_dot(struct dir_context *ctx, size_t *cur_pos,
                           const char *name, uint64_t ino) {
  size_t nl = __strlen(name);
  uint16_t r = (uint16_t)((sizeof(struct dirent64) + nl + 1 + 7) & ~7);
  if (*cur_pos < ctx->pos) {
    *cur_pos += r;
    return true;
  }
  if (!dir_emit(ctx, name, (int)nl, *cur_pos, ino, DT_DIR))
    return false;
  *cur_pos += r;
  return true;
}

/* ===== getdents（fstype 回调）===== */
static ssize_t tmpfs_getdents(struct inode *dir, struct dir_context *ctx) {
  struct tmpfs_inode_info *ti = (struct tmpfs_inode_info *)dir->i_priv;
  if (!ti)
    return 0;
  spin_lock(&ti->lock);
  /* EOF marker from previous call */
  if (ctx->pos == (uint64_t)-1) {
    spin_unlock(&ti->lock);
    return 0;
  }
  size_t cur_pos = 0;
  /* Synthetic "." and ".." precede real children (POSIX/Linux convention for
   * in-memory filesystems, which have no on-disk dot entries). */
  uint64_t parent_ino =
      ti->parent ? ti->parent->inode->ino : dir->ino; /* root: .. → self */
  if (!tmpfs_emit_dot(ctx, &cur_pos, ".", dir->ino))
    goto done;
  if (!tmpfs_emit_dot(ctx, &cur_pos, "..", parent_ino))
    goto done;
  struct tmpfs_inode_info *c = ti->children;
  while (c) {
    size_t nl = __strlen(c->name);
    unsigned dt = (c->inode && c->inode->type == INODE_DIR) ? DT_DIR : DT_REG;
    if (c->inode && c->inode->type == INODE_SOCKET)
      dt = DT_SOCK;
    uint16_t r = (uint16_t)((sizeof(struct dirent64) + nl + 1 + 7) & ~7);
    if (cur_pos < ctx->pos) {
      cur_pos += r;
      c = c->sibling;
      continue;
    }
    if (!dir_emit(ctx, c->name, (int)nl, cur_pos, c->inode->ino, dt))
      goto done;
    cur_pos += r;
    c = c->sibling;
  }
  ctx->pos = (uint64_t)-1; /* EOF: all entries emitted */
done:
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

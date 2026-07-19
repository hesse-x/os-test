/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/bsd/inode.h"
#include "kernel/bsd/page_cache.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/trap.h"
#include <stddef.h>

static struct inode *inode_hash_table[INODE_HASH_SIZE];
static spinlock inode_hash_lock = SPINLOCK_INIT;
static uint32_t next_dev_ino = 0x80000000;
/* ino 段分区（全局 hash 唯一）：
 *   FAT32     cluster 派生      (< 0x80000000)
 *   devtmpfs  next_dev_ino      (0x80000000+, 设备/目录)
 *   tmpfs     next_tmpfs_ino    (0xC0000000+, 内存 fs 文件/socket)
 * 注意：inode_create 对 INODE_DIR 强制走 next_dev_ino 自增（忽略调用者 ino），
 * 故 tmpfs 目录 inode 落在 dev 段；tmpfs 文件/socket inode 用调用者传入的
 * 0xC0000000+ ino（走 else 分支保留）。各 fs 自维护计数器，全局 hash 去重。 */

static unsigned inode_hash(uint32_t ino) { return ino & (INODE_HASH_SIZE - 1); }

void inode_init(void) {
  for (int i = 0; i < INODE_HASH_SIZE; i++)
    inode_hash_table[i] = NULL;
}

struct inode *inode_lookup(uint32_t ino) {
  unsigned idx = inode_hash(ino);
  spin_lock(&inode_hash_lock);
  struct inode *ip = inode_hash_table[idx];
  while (ip) {
    if (ip->ino == ino) {
      ASSERT(refcount_read(&ip->i_count) > 0);
      refcount_inc(&ip->i_count);
      spin_unlock(&inode_hash_lock);
      return ip;
    }
    ip = ip->hash_next;
  }
  spin_unlock(&inode_hash_lock);
  return NULL;
}

struct inode *inode_create(uint32_t ino, int type, uint64_t size,
                           uint32_t start_cluster, uint32_t dir_cluster,
                           int dir_entry_idx) {
  struct inode *ip = (struct inode *)kmalloc(sizeof(struct inode));
  if (!ip)
    return NULL;
  ip->type = type;
  /* devtmpfs directories and device nodes have no FAT32 start_cluster to
   * use as an ino, and passing 0 collides with FAT32 files whose
   * start_cluster is 0 — allocate a unique ino from the dev range. */
  ip->ino = (type == INODE_DEV || type == INODE_DIR) ? next_dev_ino++ : ino;
  /* ino=0 is a reserved sentinel (FAT32 empty files historically
   * collided here); the final hashed ino must never be 0. */
  ASSERT(ip->ino != 0);
  ip->size = size;
  ip->mode = (type == INODE_DIR)      ? 0040755
             : (type == INODE_DEV)    ? 0020000
             : (type == INODE_SOCKET) ? 0140000
                                      : 0100644;
  ip->nlink = 1;
  refcount_set(&ip->i_count, 1);
  ip->i_lock = SPINLOCK_INIT;
  ip->i_priv = NULL;
  ip->i_op = NULL; /* 未挂的 inode dispatch 安全返 -ENOSYS/-EACCES */
  ip->shm = NULL;
  ip->mount = NULL;
  ip->wq = NULL;
  ip->start_cluster = start_cluster;
  ip->dir_start_cluster = dir_cluster;
  ip->dir_entry_index = dir_entry_idx;
  ip->hash_next = NULL;
  ip->hash_prev = NULL;

  unsigned idx = inode_hash(ip->ino);
  spin_lock(&inode_hash_lock);
  ip->hash_next = inode_hash_table[idx];
  if (inode_hash_table[idx])
    inode_hash_table[idx]->hash_prev = ip;
  inode_hash_table[idx] = ip;
  spin_unlock(&inode_hash_lock);
  return ip;
}

struct inode *inode_get_or_create(uint32_t ino, int type, uint64_t size,
                                  uint32_t start_cluster, uint32_t dir_cluster,
                                  int dir_entry_idx) {
  /* ino=0 is reserved (FAT32 empty files historically collided here with
   * devtmpfs dir inodes). FAT32 now uses position-based inos which are
   * never 0; assert to catch any future regression. */
  ASSERT(ino != 0);
  unsigned idx = inode_hash(ino);
  spin_lock(&inode_hash_lock);

  /* Lookup first — if inode already exists, just increment ref */
  struct inode *ip = inode_hash_table[idx];
  while (ip) {
    if (ip->ino == ino) {
      /* The ino uniquely identifies the on-disk object, so a cache
       * hit must be the same kind of object. A mismatch means two
       * different objects mapped to the same ino (a collision bug);
       * failing here surfaces it immediately instead of silently
       * returning the wrong type and crashing far downstream. */
      ASSERT(ip->type == type);
      refcount_inc(&ip->i_count);
      spin_unlock(&inode_hash_lock);
      return ip;
    }
    ip = ip->hash_next;
  }

  /* Not found — create under lock to prevent TOCTOU race */
  ip = (struct inode *)kmalloc(sizeof(struct inode));
  if (!ip) {
    spin_unlock(&inode_hash_lock);
    return NULL;
  }
  ip->type = type;
  ip->ino = (type == INODE_DEV) ? next_dev_ino++ : ino;
  ip->size = size;
  ip->mode = (type == INODE_DIR)      ? 0040755
             : (type == INODE_DEV)    ? 0020000
             : (type == INODE_SOCKET) ? 0140000
                                      : 0100644;
  ip->nlink = 1;
  refcount_set(&ip->i_count, 1);
  ip->i_lock = SPINLOCK_INIT;
  ip->i_priv = NULL;
  ip->i_op = NULL; /* cache miss 新建;命中分支复用旧 inode,iget 出口幂等补挂 */
  ip->shm = NULL;
  ip->mount = NULL;
  ip->wq = NULL;
  ip->start_cluster = start_cluster;
  ip->dir_start_cluster = dir_cluster;
  ip->dir_entry_index = dir_entry_idx;
  ip->hash_next = inode_hash_table[idx];
  ip->hash_prev = NULL;
  if (inode_hash_table[idx])
    inode_hash_table[idx]->hash_prev = ip;
  inode_hash_table[idx] = ip;

  spin_unlock(&inode_hash_lock);
  return ip;
}

struct inode *inode_get(struct inode *ip) {
  extern volatile void *g_diag_event0_inode;
  if (g_diag_event0_inode && ip == (struct inode *)g_diag_event0_inode) {
    uint64_t pre = ip->_canary_pre, post = ip->_canary_post;
    int ic = refcount_read(&ip->i_count);
    printk(LOG_ERROR,
           "§3-DIAG: inode_get event0 %p i_count=%d canary pre=%#lx post=%#lx "
           "i_priv=%p caller pid=%d\n",
           ip, ic, (unsigned long)pre, (unsigned long)post, ip->i_priv,
           current_task->pid);
    if (ic <= 0 || pre != 0xDEADBEEFCAFEULL || post != 0xDEADBEEFCAFEULL) {
      printk(LOG_ERROR,
             "§3-DIAG: event0 inode %p CORRUPT at inode_get i_count=%d "
             "canary pre=%#lx post=%#lx i_priv=%p\n",
             ip, ic, (unsigned long)pre, (unsigned long)post, ip->i_priv);
      dump_stack_trace();
    }
  }
  ASSERT(refcount_read(&ip->i_count) > 0);
  refcount_inc(&ip->i_count);
  return ip;
}

void inode_put(struct inode *ip) {
  if (!ip)
    return;
  /* §3-DIAG: catch a premature inode_put on /dev/input/event0 — the dev_list
   * entry holds a permanent base ref and pid 5's consumer fd holds another, so
   * i_count should be ≥2 while the fd is open. If a put drives it toward 0,
   * dump the caller backtrace to locate the extra put. */
  extern volatile void *g_diag_event0_inode;
  if (g_diag_event0_inode && ip == (struct inode *)g_diag_event0_inode) {
    int before = refcount_read(&ip->i_count);
    printk(LOG_ERROR,
           "§3-DIAG: inode_put event0 %p i_count %d→%d caller pid=%d tgid=%d\n",
           ip, before, before - 1, current_task->pid, current_task->tgid);
    if (before <= 1)
      dump_stack_trace();
  }
  /* Heap-corruption check: if the canaries around i_count are stale, the
   * refcount field was overwritten by an out-of-bounds write on an adjacent
   * allocation (not a legit inode_put, which is instrumented above). Dump the
   * neighbors to locate the overflowing object. */
  if (g_diag_event0_inode && ip == (struct inode *)g_diag_event0_inode) {
    uint64_t pre = ip->_canary_pre, post = ip->_canary_post;
    if (pre != 0xDEADBEEFCAFEULL || post != 0xDEADBEEFCAFEULL) {
      printk(LOG_ERROR,
             "§3-DIAG: event0 inode %p CANARY CORRUPT pre=%#lx post=%#lx "
             "i_count=%d i_priv=%p\n",
             ip, (unsigned long)pre, (unsigned long)post,
             refcount_read(&ip->i_count), ip->i_priv);
      /* Dump the raw inode memory + preceding 64B (likely the overflowing
       * object) as 8-byte words to identify what now occupies the slot. */
      uint64_t *base = (uint64_t *)((char *)ip - 64);
      for (size_t i = 0; i < 16 + sizeof(struct inode) / 8; i++) {
        uint64_t v = base[i];
        uintptr_t a = (uintptr_t)&base[i];
        printk(LOG_ERROR, "  [%#lx]=%#016lx", (unsigned long)a,
               (unsigned long)v);
      }
      dump_stack_trace();
    }
  }
  spin_lock(&inode_hash_lock);
  if (refcount_dec_and_test(&ip->i_count)) {
    unsigned idx = inode_hash(ip->ino);
    if (inode_hash_table[idx] == ip)
      inode_hash_table[idx] = ip->hash_next;
    if (ip->hash_prev)
      ip->hash_prev->hash_next = ip->hash_next;
    if (ip->hash_next)
      ip->hash_next->hash_prev = ip->hash_prev;
    spin_unlock(&inode_hash_lock);

    /* Invalidate page cache before kfree — otherwise cp->inode becomes
     * a dangling pointer.  If slab reuses the same address for a new
     * inode, page_cache_lookup would match the stale cp by address. */
    page_cache_invalidate_inode(ip);

    if (ip->shm) {
      shm_put(ip->shm);
      ip->shm = NULL;
    }

    kfree(ip);
  } else {
    spin_unlock(&inode_hash_lock);
  }
}

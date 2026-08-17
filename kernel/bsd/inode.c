/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/bsd/inode.h"
#include "kernel/bsd/file_lock.h"
#include "kernel/bsd/page_cache.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/trap.h"
#include <stddef.h>

static struct inode *inode_hash_table[INODE_HASH_SIZE];
static spinlock inode_hash_lock = SPINLOCK_INIT;
static uint32_t next_dev_ino = 0x80000000;
// ino range partitioning (globally hash-unique):
//   FAT32     cluster-derived     (< 0x80000000)
//   devtmpfs  next_dev_ino        (0x80000000+, devices/dirs)
//   tmpfs     next_tmpfs_ino      (0xC0000000+, in-memory fs files/sockets)
// Note: inode_create forces INODE_DIR through next_dev_ino increment (ignoring
// the caller's ino), so tmpfs dir inodes land in the dev range; tmpfs
// file/socket inodes use the caller-supplied 0xC0000000+ ino (else branch
// preserves it). Each fs maintains its own counter; the global hash dedups.

static unsigned inode_hash(struct super_block *sb, uint64_t ino) {
  uintptr_t key = (uintptr_t)sb;
  key ^= key >> 7;
  key ^= (uintptr_t)ino ^ (uintptr_t)(ino >> 32);
  return (unsigned)key & (INODE_HASH_SIZE - 1);
}

void inode_init(void) {
  for (int i = 0; i < INODE_HASH_SIZE; i++)
    inode_hash_table[i] = NULL;
}

struct inode *inode_lookup(struct super_block *sb, uint64_t ino) {
  unsigned idx = inode_hash(sb, ino);
  spin_lock(&inode_hash_lock);
  struct inode *ip = inode_hash_table[idx];
  while (ip) {
    if (ip->i_sb == sb && ip->ino == ino) {
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

struct inode *inode_create(struct super_block *sb, uint64_t ino, int type,
                           uint64_t size) {
  struct inode *ip = (struct inode *)kmalloc(sizeof(struct inode));
  if (!ip)
    return NULL;
  ip->i_sb = sb;
  ip->type = type;
  // devtmpfs directories and device nodes have no FAT32 start_cluster to
  // use as an ino, and passing 0 collides with FAT32 files whose
  // start_cluster is 0 — allocate a unique ino from the dev range.
  ip->ino = ino ? ino : next_dev_ino++;
  // ino=0 is a reserved sentinel (FAT32 empty files historically
  // collided here); the final hashed ino must never be 0.
  ASSERT(ip->ino != 0);
  ip->size = size;
  // S_IFLNK | 0777
  ip->mode = (type == INODE_DIR)      ? 0040755
             : (type == INODE_DEV)    ? 0020000
             : (type == INODE_SOCKET) ? 0140000
             : (type == INODE_LNK)    ? 0120777
                                      : 0100644;
  ip->uid = 0;
  ip->gid = 0;
  ip->nlink = 1;
  refcount_set(&ip->i_count, 1);
  mutex_init(&ip->i_lock);
  ip->i_priv = NULL;
  ip->device_private = NULL;
  ip->i_op = NULL; // unmounted inode dispatch safely returns -ENOSYS/-EACCES
  ip->i_fop = NULL;
  ip->i_aop = NULL;
  ip->i_private = NULL;
  ip->shm = NULL;
  ip->mount = NULL;
  ip->wq = NULL;
  ip->release = NULL;
  ip->release_arg = NULL;
  ip->atime = ip->mtime = ip->ctime = (struct vfs_timespec64){0};
  list_init(&ip->i_flock);
  ip->i_flock_lock = SPINLOCK_INIT;
  ip->hash_next = NULL;
  ip->hash_prev = NULL;

  unsigned idx = inode_hash(ip->i_sb, ip->ino);
  spin_lock(&inode_hash_lock);
  ip->hash_next = inode_hash_table[idx];
  if (inode_hash_table[idx])
    inode_hash_table[idx]->hash_prev = ip;
  inode_hash_table[idx] = ip;
  spin_unlock(&inode_hash_lock);
  return ip;
}

struct inode *inode_get_or_create(struct super_block *sb, uint64_t ino,
                                  int type, uint64_t size) {
  // ino=0 is reserved (FAT32 empty files historically collided here with
  // devtmpfs dir inodes). FAT32 now uses position-based inos which are
  // never 0; assert to catch any future regression.
  ASSERT(ino != 0);
  unsigned idx = inode_hash(sb, ino);
  spin_lock(&inode_hash_lock);

  // Lookup first — if inode already exists, just increment ref
  struct inode *ip = inode_hash_table[idx];
  while (ip) {
    if (ip->i_sb == sb && ip->ino == ino) {
      // The ino uniquely identifies the on-disk object, so a cache
      // hit must be the same kind of object. A mismatch means two
      // different objects mapped to the same ino (a collision bug);
      // failing here surfaces it immediately instead of silently
      // returning the wrong type and crashing far downstream.
      ASSERT(ip->type == type);
      refcount_inc(&ip->i_count);
      spin_unlock(&inode_hash_lock);
      return ip;
    }
    ip = ip->hash_next;
  }

  // Not found — create under lock to prevent TOCTOU race
  ip = (struct inode *)kmalloc(sizeof(struct inode));
  if (!ip) {
    spin_unlock(&inode_hash_lock);
    return NULL;
  }
  ip->i_sb = sb;
  ip->type = type;
  ip->ino = ino ? ino : next_dev_ino++;
  ip->size = size;
  // S_IFLNK | 0777
  ip->mode = (type == INODE_DIR)      ? 0040755
             : (type == INODE_DEV)    ? 0020000
             : (type == INODE_SOCKET) ? 0140000
             : (type == INODE_LNK)    ? 0120777
                                      : 0100644;
  ip->uid = 0;
  ip->gid = 0;
  ip->nlink = 1;
  refcount_set(&ip->i_count, 1);
  mutex_init(&ip->i_lock);
  ip->i_priv = NULL;
  ip->device_private = NULL;
  ip->i_op = NULL; // cache miss new inode; the hit branch reuses the old one,
                   // iget attaches i_op idempotently at the exit
  ip->i_fop = NULL;
  ip->i_aop = NULL;
  ip->i_private = NULL;
  ip->shm = NULL;
  ip->mount = NULL;
  ip->wq = NULL;
  ip->release = NULL;
  ip->release_arg = NULL;
  ip->atime = ip->mtime = ip->ctime = (struct vfs_timespec64){0};
  list_init(&ip->i_flock);
  ip->i_flock_lock = SPINLOCK_INIT;
  ip->hash_next = inode_hash_table[idx];
  ip->hash_prev = NULL;
  if (inode_hash_table[idx])
    inode_hash_table[idx]->hash_prev = ip;
  inode_hash_table[idx] = ip;

  spin_unlock(&inode_hash_lock);
  return ip;
}

struct inode *inode_get(struct inode *ip) {
  ASSERT(refcount_read(&ip->i_count) > 0);
  refcount_inc(&ip->i_count);
  return ip;
}

void inode_for_each(inode_iter_fn fn, void *ctx) {
  // Snapshot-walk the hash table under inode_hash_lock. fn is expected to be
  // cheap (lock-list mutation) and must not drop the inode's refcount to zero
  // (which would kfree it under us mid-walk); S09's pid cleanup only removes
  // file_lock nodes, never the inode itself.
  spin_lock(&inode_hash_lock);
  for (unsigned b = 0; b < INODE_HASH_SIZE; b++) {
    for (struct inode *ip = inode_hash_table[b]; ip; ip = ip->hash_next)
      fn(ip, ctx);
  }
  spin_unlock(&inode_hash_lock);
}

void inode_put(struct inode *ip) {
  if (!ip)
    return;
  spin_lock(&inode_hash_lock);
  if (refcount_dec_and_test(&ip->i_count)) {
    unsigned idx = inode_hash(ip->i_sb, ip->ino);
    if (inode_hash_table[idx] == ip)
      inode_hash_table[idx] = ip->hash_next;
    if (ip->hash_prev)
      ip->hash_prev->hash_next = ip->hash_next;
    if (ip->hash_next)
      ip->hash_next->hash_prev = ip->hash_prev;
    spin_unlock(&inode_hash_lock);

    // Invalidate page cache before kfree — otherwise cp->inode becomes
    // a dangling pointer.  If slab reuses the same address for a new
    // inode, page_cache_lookup would match the stale cp by address.
    page_cache_invalidate_inode(ip);

    if (ip->i_sb && ip->i_sb->s_op && ip->i_sb->s_op->evict_inode)
      ip->i_sb->s_op->evict_inode(ip);

    if (ip->shm) {
      shm_put(ip->shm);
      ip->shm = NULL;
    }

    // Drop any POSIX file locks still held against this inode. A live process
    // should have released them on exit/close, but if an inode is evicted while
    // locks remain (e.g. a process crashed without running cleanup) free them
    // here rather than leaking the file_lock nodes.
    file_lock_release_all(ip);

    if (ip->release)
      ip->release(ip, ip->release_arg);

    kfree(ip);
  } else {
    spin_unlock(&inode_hash_lock);
  }
}

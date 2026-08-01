/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_INODE_H
#define KERNEL_INODE_H

#include <stddef.h>
#include <stdint.h>

#include "kernel/xcore/atomic.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/wait_queue.h"

#define INODE_REGULAR 1
#define INODE_DIR 2
#define INODE_DEV 3
#define INODE_SOCKET 4
#define INODE_LNK                                                              \
  5 // symlink: target string stored per-fs (i_priv or fs-private data)        \
     //

// utimensat / update_time selection bits: mark which timestamp fields to update
// (matches Linux inode_operations.update_time mask semantics).
#define ATIME_BIT 0x1
#define MTIME_BIT 0x2
#define CTIME_BIT 0x4

struct inode;
struct kstat;

// per-inode behavior table (mirrors Linux struct inode_operations, trimmed).
// lookup/create/mkdir/unlink/rmdir dispatch from the parent directory's i_op;
// getattr/setattr dispatch from the target inode's i_op.
// read/write/getdents belong to f_op/data layer, not this table (§6.1).
struct inode_operations {
  struct inode *(*lookup)(struct inode *dir, const char *name);
  struct inode *(*create)(struct inode *dir, const char *name, int mode);
  int (*mkdir)(struct inode *dir, const char *name, int mode);
  int (*unlink)(struct inode *dir, const char *name);
  int (*rmdir)(struct inode *dir, const char *name);
  // rename: move the old_name node under old_dir to new_name under new_dir.
  // Full rename(2) semantics (matches Linux): same/cross-directory atomic;
  // new exists → atomic replace; directory boundary checks
  // (ENOTEMPTY/EISDIR/ENOTDIR/EINVAL cycle); old==new no-op; open fds
  // unaffected (inode refcount). NULL → -EPERM.
  int (*rename)(struct inode *old_dir, const char *old_name,
                struct inode *new_dir, const char *new_name);
  int (*getattr)(struct inode *ip, struct kstat *ks);
  int (*setattr)(struct inode *ip, uint64_t size);
  // symlink / hard link / permission / timestamp (Linux inode_operations
  // subset). Each fs attaches what it supports; NULL → VFS layer falls back to
  // the generic implementation or returns -ENOSYS/-EPERM.
  //   symlink(dir,name,target): create an LNK inode named name under dir
  //   pointing to target link(dir,target,newname): create a hard link named
  //   newname under dir (target inode) readlink(ip,buf,bufsiz): copy the target
  //   string to buf, return length (no NUL terminator) permission(ip,mask):
  //   0=allow, negative=-errno; NULL → VFS generic inode_permission
  //   update_time(ip,at,mt,ct,which): write at/mt/ct per which bits; NULL →
  //   generic
  struct inode *(*symlink)(struct inode *dir, const char *name,
                           const char *target);
  int (*link)(struct inode *dir, struct inode *target, const char *newname);
  int (*readlink)(struct inode *ip, char *buf, size_t bufsiz);
  int (*permission)(struct inode *ip, int mask);
  int (*update_time)(struct inode *ip, uint64_t at, uint64_t mt, uint64_t ct,
                     int which);
};

struct inode {
  int type;
  uint32_t ino;
  uint64_t size;
  uint32_t mode;
  uint32_t
      uid; // owner uid (set to creator's uid; existing inodes default to 0)
  uint32_t
      gid; // owner gid (set to creator's gid; existing inodes default to 0)
  int nlink;
  refcount_t i_count;
  spinlock i_lock;
  void *i_priv; /* INODE_DEV -> dev_ops*; INODE_REGULAR -> NULL */
  const struct inode_operations
      *i_op; // behavior table (attached at iget exit); unmounted → dispatch
             // returns -ENOSYS/-EACCES
  struct shm *shm;           /* INODE_DEV -> shared memory (NULL = no SHM) */
  struct mount_entry *mount; /* owning mount (set by sys_open lookup) */
  wait_queue_head *wq; /* ringbuf-backed: shared wq for epoll/poll waiters */

  /* POSIX file locks (S09): per-inode lock list + its own spinlock (independent
   * of i_lock, which guards FAT32 metadata — flock ops never touch metadata).
   */
  list_node i_flock;     /* head of file_lock list (list_init on create) */
  spinlock i_flock_lock; /* protects i_flock */

  /* FAT32 metadata (REGULAR/DIR only) */
  uint32_t start_cluster;
  uint32_t dir_start_cluster;
  int dir_entry_index;

  /* POSIX timestamps in ns since epoch (CLOCK_REALTIME). In-memory only —
   * FAT32 stores no timestamps (Q5: llvm libc utimensat tests don't survive
   * reboot); getattr reads these. Updated by update_time / generic_update_time.
   */
  uint64_t atime;
  uint64_t mtime;
  uint64_t ctime;

  /* Hash chain */
  struct inode *hash_next;
  struct inode *hash_prev;
};

#define INODE_HASH_BITS 6
#define INODE_HASH_SIZE (1 << INODE_HASH_BITS) /* 64 */

void inode_init(void);
struct inode *inode_lookup(uint32_t ino);
struct inode *inode_create(uint32_t ino, int type, uint64_t size,
                           uint32_t start_cluster, uint32_t dir_cluster,
                           int dir_entry_idx) __must_check;
struct inode *inode_get_or_create(uint32_t ino, int type, uint64_t size,
                                  uint32_t start_cluster, uint32_t dir_cluster,
                                  int dir_entry_idx);
void inode_put(struct inode *ip);
struct inode *inode_get(struct inode *ip);

/* Walk every cached inode, calling fn(ip, ctx) for each. Used by S09 file-lock
 * cleanup to release a dying process's POSIX locks across all inodes without
 * exposing the static hash table. fn must not block on inode eviction. */
typedef void (*inode_iter_fn)(struct inode *ip, void *ctx);
void inode_for_each(inode_iter_fn fn, void *ctx);

#endif

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
  5 /* 符号链接:target 串由各 fs 自存(i_priv 或 fs 私有数据)     \
     */

/* utimensat / update_time 选择位:标记要更新的时间戳字段(对齐 Linux
 * inode_operations.update_time 的 mask 语义)。 */
#define ATIME_BIT 0x1
#define MTIME_BIT 0x2
#define CTIME_BIT 0x4

struct inode;
struct kstat;

/* per-inode 行为表(对齐 Linux struct inode_operations,裁剪子集)。
 * lookup/create/mkdir/unlink/rmdir 由父目录 inode 的 i_op 分发;
 * getattr/setattr 由目标 inode 的 i_op 分发。
 * read/write/getdents 属 f_op/数据层,不在本表(§6.1)。 */
struct inode_operations {
  struct inode *(*lookup)(struct inode *dir, const char *name);
  struct inode *(*create)(struct inode *dir, const char *name, int mode);
  int (*mkdir)(struct inode *dir, const char *name, int mode);
  int (*unlink)(struct inode *dir, const char *name);
  int (*rmdir)(struct inode *dir, const char *name);
  /* rename:将 old_dir 下 old_name 节点移到 new_dir 下 new_name。
   * 完整 rename(2) 语义(对齐 Linux):同/跨目录均支持且原子;new 存在
   * 原子替换;目录边界(ENOTEMPTY/EISDIR/ENOTDIR/EINVAL 循环);old==new
   * no-op;已 open fd 不受影响(inode 引用计数)。NULL → -EPERM。 */
  int (*rename)(struct inode *old_dir, const char *old_name,
                struct inode *new_dir, const char *new_name);
  int (*getattr)(struct inode *ip, struct kstat *ks);
  int (*setattr)(struct inode *ip, uint64_t size);
  /* 符号链接 / 硬链接 / 权限 / 时间戳(对齐 Linux inode_operations 子集)。
   * 各 fs 按能力挂载;NULL → VFS 层回退到通用实现或返 -ENOSYS/-EPERM。
   *   symlink(dir,name,target):在 dir 下建名为 name、指向 target 的 LNK inode
   *   link(dir,target,newname):在 dir 下建名为 newname 的硬链(target inode)
   *   readlink(ip,buf,bufsiz):拷出 target 串到 buf,返回长度(不 NUL 终止)
   *   permission(ip,mask):0=允许,负=-errno;NULL → VFS 通用 inode_permission
   *   update_time(ip,at,mt,ct,which):按 which 位写 at/mt/ct;NULL → generic */
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
  uint32_t uid; /* owner uid (创建时设为创建进程 uid;存量 inode 默认 0) */
  uint32_t gid; /* owner gid (创建时设为创建进程 gid;存量 inode 默认 0) */
  int nlink;
  refcount_t i_count;
  spinlock i_lock;
  void *i_priv; /* INODE_DEV -> dev_ops*; INODE_REGULAR -> NULL */
  const struct inode_operations
      *i_op; /* 行为表(iget 出口挂);未挂则 dispatch 返 -ENOSYS/-EACCES */
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

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
// tmpfs: in-memory filesystem backing /run and /dev/shm. Cleared on reboot;
// process crashes do not affect it. ino numbers are self-allocated from the
// 0xC0000000 range, avoiding the devtmpfs 0x80000000 and FAT32 cluster ranges.
// Note: INODE_DIR nodes go through inode_create which forces next_dev_ino (the
// dev range); only files/sockets fall into the 0xC0000000+ range. Each range is
// deduplicated by the global inode hash, so no cross-range coordination is
// needed.

#include "kernel/bsd/tmpfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arch/x64/utils.h"
#include "kernel/bsd/fops.h"
#include "kernel/bsd/inode.h"
#include "kernel/bsd/inotify.h"
#include "kernel/bsd/mount.h"
#include "kernel/bsd/types.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/kpi.h"
#include "kernel/xcore/mem/kasan.h"
#include "kernel/xcore/mm_types.h"
#include "kernel/xcore/mutex.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/spinlock.h"

#include <kernel/bsd/stat_abi.h>
#include <xos/dirent.h>
#include <xos/errno.h>
#include <xos/page.h>

struct xtask;

// ===== private inode data, hung on inode->i_priv =====
// Regular files/sockets: data holds the content; directories: children is the
// linked-list head.
// A socket inode's i_priv is re-pointed to a unix_sock* at bind time by the
// socket layer (see socket.c).
struct tmpfs_inode_info {
  struct inode *inode; // back pointer
  struct tmpfs_inode_info *parent;
  struct tmpfs_inode_info *children; // head-inserted
  struct tmpfs_inode_info *sibling;
  char name[256]; // directory entry name (empty for root)
  void *data;     // regular file content
  size_t size;
  size_t cap;
  spinlock lock; // protects data/size/children
};

static uint32_t next_tmpfs_ino = 0xC0000000;
static spinlock tmpfs_ino_lock = SPINLOCK_INIT;

// Capacity limits
#define TMPFS_FILE_CAP (64 * 1024)        // single file 64KB
#define TMPFS_TOTAL_CAP (1 * 1024 * 1024) // total 1MB
static size_t tmpfs_total_used = 0;
static spinlock tmpfs_total_lock = SPINLOCK_INIT;

// tmpfs global serialization lock (equivalent to Linux's s_vfs_rename_mutex;
// serializes all tmpfs renames to prevent cross-directory deadlocks). This OS
// has no semaphore/mutex, so a spinlock_t + irqsave is used. The db scenario
// only uses the same directory, but the interface matches Linux so
// cross-directory support is required.
static spinlock tmpfs_rename_lock = SPINLOCK_INIT;

static uint32_t tmpfs_alloc_ino(void) {
  spin_lock(&tmpfs_ino_lock);
  uint32_t ino = next_tmpfs_ino++;
  spin_unlock(&tmpfs_ino_lock);
  return ino;
}

// Create a tmpfs_inode_info and attach it to a new inode->i_priv
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

// ===== i_op callbacks =====
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
// §3.3 symlink: create a LNK inode named `name` under dir, pointing at target.
static struct inode *tmpfs_symlink(struct inode *dir, const char *name,
                                   const char *target);
// §3.3 readlink: copy the LNK inode's target string into buf, return its length
// (not NUL-terminated).
static int tmpfs_readlink(struct inode *ip, char *buf, size_t bufsiz);
// §3.4 link: create a hard link named newname to target under dir. target must
// not be a directory (POSIX EPERM), must be same fs (EXDEV), duplicate name is
// EEXIST. The directory entry holds a new inode reference + nlink++.
static int tmpfs_link(struct inode *dir, struct inode *target,
                      const char *newname);

static const struct inode_operations tmpfs_dir_iop = {
    .lookup = tmpfs_lookup,
    .create = tmpfs_create,
    .mkdir = tmpfs_mkdir,
    .unlink = tmpfs_unlink,
    .rmdir = tmpfs_rmdir,
    .rename = tmpfs_rename, // new in this plan
    .getattr = tmpfs_getattr,
    .setattr = tmpfs_setattr,
    .symlink = tmpfs_symlink, // §3.3 symlink
    .link = tmpfs_link,       // §3.4 hard link
};

static const struct inode_operations tmpfs_file_iop = {
    .getattr = tmpfs_getattr,
    .setattr = tmpfs_setattr,
};

// LNK inode iop: readlink reads the target (stored in tmpfs_inode_info.data);
// getattr/setattr reuse the file version (LNK inode size = target string
// length).
static const struct inode_operations tmpfs_lnk_iop = {
    .getattr = tmpfs_getattr,
    .setattr = tmpfs_setattr,
    .readlink = tmpfs_readlink,
};

// ===== i_op implementations =====
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
  // S08: st_uid/st_gid now report the real ip->uid/gid (set at creation by
  // sys_open/mkdir/mknod). Timestamps (Q5): in-memory atime/mtime/ctime,
  // getattr reads out ns and splits into sec/nsec. st_rdev is still left 0.
  ks->st_atim.tv_sec = ip->atime.tv_sec;
  ks->st_atim.tv_nsec = ip->atime.tv_nsec;
  ks->st_mtim.tv_sec = ip->mtime.tv_sec;
  ks->st_mtim.tv_nsec = ip->mtime.tv_nsec;
  ks->st_ctim.tv_sec = ip->ctime.tv_sec;
  ks->st_ctim.tv_nsec = ip->ctime.tv_nsec;
  return 0;
}

static void *tmpfs_page_data(struct inode *ip, size_t page_index) {
  struct shm *shm = ip->shm;
  uint64_t phys = shm->page_list ? shm->page_list[page_index]
                                 : shm->phys + page_index * PAGE_SIZE;
  return (__force void *)phys_to_virt((__force phys_addr_t)phys);
}

static int tmpfs_grow_locked(struct inode *ip, struct tmpfs_inode_info *ti,
                             size_t size) {
  if (size <= ti->cap)
    return 0;

  size_t delta = size - ti->cap;
  spin_lock(&tmpfs_total_lock);
  if (tmpfs_total_used + delta > TMPFS_TOTAL_CAP) {
    spin_unlock(&tmpfs_total_lock);
    return -ENOSPC;
  }
  tmpfs_total_used += delta;
  spin_unlock(&tmpfs_total_lock);

  size_t npages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
  int rc = 0;
  if (!ip->shm) {
    ip->shm = shm_create_internal(npages);
    if (!ip->shm)
      rc = -ENOMEM;
  } else if (shm_grow(ip->shm, npages) != 0) {
    rc = -ENOMEM;
  }
  if (rc != 0) {
    spin_lock(&tmpfs_total_lock);
    tmpfs_total_used -= delta;
    spin_unlock(&tmpfs_total_lock);
    return rc;
  }

  ti->cap = size;
  return 0;
}

static void tmpfs_zero_locked(struct inode *ip, size_t offset, size_t count) {
  while (count > 0) {
    size_t page_off = offset & (PAGE_SIZE - 1);
    size_t n = PAGE_SIZE - page_off;
    if (n > count)
      n = count;
    __memset((char *)tmpfs_page_data(ip, offset / PAGE_SIZE) + page_off, 0, n);
    offset += n;
    count -= n;
  }
}

static int tmpfs_copy_out_locked(struct inode *ip, uint64_t offset, void *buf,
                                 size_t count, bool user) {
  size_t done = 0;
  while (done < count) {
    size_t page_off = (size_t)offset & (PAGE_SIZE - 1);
    size_t n = PAGE_SIZE - page_off;
    if (n > count - done)
      n = count - done;
    void *src =
        (char *)tmpfs_page_data(ip, (size_t)offset / PAGE_SIZE) + page_off;
    if (user) {
      if (copy_to_user((char *)buf + done, src, n) != 0)
        return -EFAULT;
    } else {
      __memcpy((char *)buf + done, src, n);
    }
    offset += n;
    done += n;
  }
  return 0;
}

static int tmpfs_copy_in_locked(struct inode *ip, size_t offset,
                                const void *buf, size_t count) {
  size_t done = 0;
  while (done < count) {
    size_t page_off = offset & (PAGE_SIZE - 1);
    size_t n = PAGE_SIZE - page_off;
    if (n > count - done)
      n = count - done;
    void *dst = (char *)tmpfs_page_data(ip, offset / PAGE_SIZE) + page_off;
    if (copy_from_user(dst, (const char *)buf + done, n) != 0)
      return -EFAULT;
    offset += n;
    done += n;
  }
  return 0;
}

static int tmpfs_setattr(struct inode *ip, uint64_t size) {
  struct tmpfs_inode_info *ti = (struct tmpfs_inode_info *)ip->i_priv;
  if (!ti)
    return -EFAULT;
  spin_lock(&ti->lock);
  if (ip->type != INODE_REGULAR) {
    spin_unlock(&ti->lock);
    return -EINVAL;
  }
  if (size > TMPFS_FILE_CAP) {
    spin_unlock(&ti->lock);
    return -ENOSPC;
  }
  int rc = tmpfs_grow_locked(ip, ti, (size_t)size);
  if (rc != 0) {
    spin_unlock(&ti->lock);
    return rc;
  }
  if (size < ti->size && ip->shm)
    tmpfs_zero_locked(ip, (size_t)size, ti->cap - (size_t)size);
  ti->size = size;
  ip->size = size;
  spin_unlock(&ti->lock);
  return 0;
}

struct shm *tmpfs_get_shm(struct inode *ip) {
  struct tmpfs_inode_info *ti = (struct tmpfs_inode_info *)ip->i_priv;
  if (!ti)
    return NULL;
  spin_lock(&ti->lock);
  struct shm *shm = shm_get(ip->shm);
  spin_unlock(&ti->lock);
  return shm;
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

// Internal node creation: allocate ino by type + attach i_op + create and
// attach tmpfs_inode_info to the parent's children. The create path writes the
// caller's mode (with S_IFSOCK/S_IFIFO type bits + permission bits) into
// ip->mode, overriding inode_create's default mode so stat returns it verbatim
// (matching Linux mknod).
static struct inode *tmpfs_new_node(struct tmpfs_inode_info *parent_ti,
                                    const char *name, int type, int mode,
                                    int keep_mode) {
  uint32_t ino = tmpfs_alloc_ino();
  struct super_block *sb =
      parent_ti && parent_ti->inode ? parent_ti->inode->i_sb : NULL;
  struct inode *ip = inode_create(sb, ino, type, 0);
  if (!ip)
    return NULL;
  if (keep_mode)
    ip->mode = (uint32_t)mode;
  ip->i_op = (type == INODE_DIR)   ? &tmpfs_dir_iop
             : (type == INODE_LNK) ? &tmpfs_lnk_iop
                                   : &tmpfs_file_iop;
  ip->i_fop = type == INODE_REGULAR ? &tmpfs_file_fops : NULL;
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
  // Attach to parent children chain (head insert)
  if (parent_ti) {
    ti->sibling = parent_ti->children;
    parent_ti->children = ti;
    // The directory entry holds one inode reference (matching the Linux
    // dentry→inode model): inode_create exits with i_count=1 as the +1 returned
    // to the caller for create/mkdir, so another +1 is taken here to keep the
    // directory entry itself. Otherwise the caller (sys_mknod/open etc.) put
    // would drop the returned +1 and i_count→0 triggers an inode kfree, leaving
    // a dangling pointer in the children chain — a later lookup→inode_get would
    // hit the i_count==0 ASSERT and PANIC. fat32 has no such problem: its
    // directory entries live in on-disk clusters and lookup re-fetches a live
    // inode via inode_get_or_create.
    inode_get(ip);
  }
  return ip;
}

static struct inode *tmpfs_create(struct inode *dir, const char *name,
                                  int mode) {
  struct tmpfs_inode_info *parent_ti = (struct tmpfs_inode_info *)dir->i_priv;
  if (!parent_ti)
    return ERR_PTR(-EFAULT);
  // Duplicate-name check: tmpfs_lookup returns a +1 reference, must put to
  // balance
  struct inode *exist = tmpfs_lookup(dir, name);
  if (exist) {
    inode_put(exist);
    return ERR_PTR(-EEXIST);
  }
  // mode & S_IFMT: S_IFSOCK creates INODE_SOCKET; S_IFIFO is carried as a
  // regular file for now (no pipe fs), keeping the S_IFIFO bit for stat to
  // distinguish; everything else creates INODE_REGULAR. S08: the mode passed in
  // by open(O_CREAT) has only permission bits (no type bits), so
  // tmpfs_new_node writing ip->mode verbatim with keep_mode=1 would drop
  // S_IFREG and break stat's S_ISREG; here the S_IFREG type bit is added back
  // for regular files (sockets keep S_IFSOCK passed by mknod).
  int type = INODE_REGULAR;
  if ((mode & S_IFMT) == S_IFSOCK)
    type = INODE_SOCKET;
  if (type == INODE_REGULAR && (mode & S_IFMT) == 0)
    mode = S_IFREG | (mode & 0777);
  struct inode *ip = tmpfs_new_node(parent_ti, name, type, mode, 1);
  if (!ip)
    return ERR_PTR(-ENOMEM);
  // IN_CREATE on the parent dir (no ti->lock held here).
  inotify_inode_event(dir, IN_CREATE, 0, name);
  return ip; // i_count=2: 1 directory entry + 1 returned to the caller (put by
             // the caller)
}

// §3.3 tmpfs_symlink: create a LNK inode named `name` pointing at target under
// dir. The target string is stored in tmpfs_inode_info.data (NUL-terminated);
// ip->size = strlen(target), so getattr reports st_size = target length and
// readlink reads ip->size as the length. i_count=2 (matches tmpfs_create):
// 1 directory entry + 1 returned to the caller (put by the caller).
static struct inode *tmpfs_symlink(struct inode *dir, const char *name,
                                   const char *target) {
  struct tmpfs_inode_info *parent_ti = (struct tmpfs_inode_info *)dir->i_priv;
  if (!parent_ti)
    return ERR_PTR(-EFAULT);
  // Duplicate-name check: tmpfs_lookup returns a +1 reference, must put to
  // balance
  struct inode *exist = tmpfs_lookup(dir, name);
  if (exist) {
    inode_put(exist);
    return ERR_PTR(-EEXIST);
  }
  size_t tlen = __strlen(target);
  if (tlen >= 256) // RELPATH_MAX: kernel path resolution is 256 bytes; longer
                   // symlink targets are meaningless
    return ERR_PTR(-ENAMETOOLONG);
  struct inode *ip = tmpfs_new_node(parent_ti, name, INODE_LNK, 0, 0);
  if (!ip)
    return ERR_PTR(-ENOMEM);
  struct tmpfs_inode_info *ti = (struct tmpfs_inode_info *)ip->i_priv;
  ti->data = kmalloc(tlen + 1);
  if (!ti->data) {
    inode_put(ip); // drop the directory-entry ref + caller ref → 0 triggers
                   // inode kfree
    return ERR_PTR(-ENOMEM);
  }
  __memcpy(ti->data, target, tlen + 1);
  ti->size = tlen;
  ti->cap = tlen + 1;
  ip->size = tlen;
  return ip; // i_count=2
}

// §3.3 tmpfs_readlink: copy the LNK inode's target string into buf, return the
// length (POSIX: not NUL-terminated). The target is stored in
// tmpfs_inode_info.data, its length = ip->size. bufsiz truncation is handled by
// the caller (sys_readlink); here copy min(size,bufsiz) bytes per bufsiz.
static int tmpfs_readlink(struct inode *ip, char *buf, size_t bufsiz) {
  if (!ip || ip->type != INODE_LNK)
    return -EINVAL;
  struct tmpfs_inode_info *ti = (struct tmpfs_inode_info *)ip->i_priv;
  if (!ti || !ti->data)
    return -EIO; // i_priv should point at the info holding target; no data is
                 // treated as an I/O error
  size_t n = (ti->size < bufsiz) ? ti->size : bufsiz;
  __memcpy(buf, ti->data, n);
  return (int)n;
}

// §3.4 tmpfs_link: create a hard link named newname to target under dir.
// Mirrors Linux tmpfs_link:
//   - target must not be a directory (POSIX: hard-linking a directory → EPERM;
//     root-only, this OS doesn't allow it)
//   - duplicate name → EEXIST
//   - create a new tmpfs_inode_info for the new directory entry,
//   inode_get(target)
//     to hold the directory-entry reference, and target->nlink++
//     (directory-entry count).
// Cross-fs is guaranteed by do_linkat: target and newdir share a mount (both
// path_walk and path_walk_parent resolve within the same mount); a truly
// cross-fs link (linking a FAT32 file to tmpfs) has a target from the fat32
// mount and dispatch goes to the fat32 iop (link==NULL) → do_linkat returns
// EPERM. So no mount comparison is needed here (and inode.mount is set lazily —
// initialized NULL by inode_create, only set on sys_open/stat paths; comparing
// would misjudge as EXDEV).
static int tmpfs_link(struct inode *dir, struct inode *target,
                      const char *newname) {
  if (!dir || !target || !newname)
    return -EFAULT;
  if (target->type == INODE_DIR)
    return -EPERM;
  struct tmpfs_inode_info *parent_ti = (struct tmpfs_inode_info *)dir->i_priv;
  if (!parent_ti)
    return -EFAULT;
  // Duplicate-name check: tmpfs_lookup returns a +1 reference, must put to
  // balance.
  struct inode *exist = tmpfs_lookup(dir, newname);
  if (exist) {
    inode_put(exist);
    return -EEXIST;
  }
  // Create a new info for the new directory entry (head-insert into
  // parent_ti->children). data/size are left empty — a hard-link directory
  // entry is a name→inode mapping; the content comes from the target inode
  // itself, the new info holds no data.
  struct tmpfs_inode_info *ti = new_tmpfs_info(target, parent_ti);
  if (!ti)
    return -ENOMEM;
  int i = 0;
  while (newname[i] && i < 255) {
    ti->name[i] = newname[i];
    i++;
  }
  ti->name[i] = '\0';
  // The directory entry holds a reference to the target inode (matches
  // tmpfs_new_node:250-260): inode_get keeps the inode alive while the file
  // exists, sharing the same inode with the original directory entry.
  inode_get(target);
  spin_lock(&parent_ti->lock);
  ti->sibling = parent_ti->children;
  parent_ti->children = ti;
  spin_unlock(&parent_ti->lock);
  // nlink++: directory-entry count. i_lock protects nlink (matching the i_lock
  // use in setattr/getattr).
  mutex_lock(&target->i_lock);
  target->nlink++;
  mutex_unlock(&target->i_lock);
  return 0;
}

static int tmpfs_mkdir(struct inode *dir, const char *name, int mode) {
  (void)mode;
  struct tmpfs_inode_info *parent_ti = (struct tmpfs_inode_info *)dir->i_priv;
  if (!parent_ti)
    return -EFAULT;
  // Duplicate-name check: tmpfs_lookup returns a +1 reference, must put to
  // balance
  struct inode *exist = tmpfs_lookup(dir, name);
  if (exist) {
    inode_put(exist);
    return -EEXIST;
  }
  struct inode *ip = tmpfs_new_node(parent_ti, name, INODE_DIR, 0, 0);
  if (!ip)
    return -ENOMEM;
  inode_put(ip); // mkdir returns no inode; balance tmpfs_new_node's exit +1
                 // return reference
  // §3.4 nlink maintenance: a new subdirectory increments the parent's nlink
  // (directory self-refs "."/".." + child count); the new directory itself gets
  // nlink=2 ("." self-ref + ".." pointing at parent). Matches Linux
  // tmpfs_mkdir.
  mutex_lock(&dir->i_lock);
  dir->nlink++;
  mutex_unlock(&dir->i_lock);
  mutex_lock(&ip->i_lock);
  ip->nlink = 2;
  mutex_unlock(&ip->i_lock);
  // IN_CREATE on the parent dir (no ti->lock/i_lock held here).
  inotify_inode_event(dir, IN_CREATE, 0, name);
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
      // Detach the directory entry: only reclaim tmpfs_inode_info immediately
      // when the inode has no other references (i_count==1); otherwise keep it
      // (unlink-while-open / bind-held-ref scenarios), and reference counting
      // governs release. Note: a socket inode's i_priv may be a unix_sock*
      // here, not reclaimed here (the socket layer's unix_bind_unregister
      // handles it).
      struct inode *ip = c->inode;
      int last = (refcount_read(&ip->i_count) == 1);
      int is_dir = (ip->type == INODE_DIR);
      // §3.4 nlink maintenance: unlink detaches a directory entry → ip->nlink--
      // (matching link's ++). Adjusted before releasing the lock to avoid
      // racing getattr/stat's i_lock. Reaching 0 reclaims via inode_put
      // (directory-entry reference released); nlink is no longer read after.
      if (!is_dir) {
        mutex_lock(&ip->i_lock);
        if (ip->nlink > 0) // defensive: should not be 0 (an unlinked entry is
                           // no longer findable)
          ip->nlink--;
        mutex_unlock(&ip->i_lock);
      }
      spin_unlock(&parent_ti->lock);
      // IN_DELETE on the parent (name) + IN_DELETE_SELF on the child. ti->lock
      // released above; ip still holds its ref until the inode_put below.
      inotify_inode_event(dir, IN_DELETE, 0, name);
      inotify_inode_event(ip, IN_DELETE_SELF, 0, NULL);
      inode_put(ip); // detach the directory-entry reference; if i_count hits 0
                     // it triggers an inode kfree
      if (last && !is_dir) {
        // Regular file with no open fd: reclaim data (a socket inode's i_priv
        // belongs to the socket layer)
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

// tmpfs_rename: full rename(2) semantics (matching Linux). The db atomic-write
// base (§3.1). Atomicity relies on spinlock mutual exclusion (no dentry cache
// needed): same-directory uses one ti->lock critical section; cross-directory
// serializes with the global tmpfs_rename_lock and acquires the two ti->locks
// sorted by inode address. Reclamation discipline matches tmpfs_unlink:295-310:
// inode_put/kfree happen outside the ti->lock critical section.
static int tmpfs_rename(struct inode *old_dir, const char *old_name,
                        struct inode *new_dir, const char *new_name) {
  if (!old_dir || !old_name || !new_dir || !new_name)
    return -EFAULT;

  // old == new: no-op matching Linux. Explicitly handled to avoid the
  // "detach old + new(=old itself) → rename" logic deleting itself.
  if (old_dir == new_dir && __strcmp(old_name, new_name) == 0)
    return 0;

  struct tmpfs_inode_info *old_ti = (struct tmpfs_inode_info *)old_dir->i_priv;
  struct tmpfs_inode_info *new_ti = (struct tmpfs_inode_info *)new_dir->i_priv;
  if (!old_ti || !new_ti)
    return -EFAULT;

  // Global serialization lock (cross-directory deadlock avoidance +
  // atomicity). The db scenario only needs a single lock for same-directory,
  // but the interface matches Linux rename(2) so cross-directory atomicity must
  // be supported. irqsave guards against slab allocation's interrupts nesting
  // with this lock.
  uint64_t rflags;
  spin_lock_irqsave(&tmpfs_rename_lock, &rflags);

  // Acquire the two ti->locks of old_dir/new_dir sorted by inode address
  // (prevents a→b vs b→a lock-order deadlock; matches Linux vfs_rename's lock
  // ordering protocol).
  struct tmpfs_inode_info *first_ti = (old_ti < new_ti) ? old_ti : new_ti;
  struct tmpfs_inode_info *second_ti = (old_ti < new_ti) ? new_ti : old_ti;
  spin_lock(&first_ti->lock);
  if (first_ti != second_ti)
    spin_lock(&second_ti->lock);

  // Info to reclaim (handled after unlocking, matching tmpfs_unlink
  // reclamation discipline)
  struct inode *reclaim_ip = NULL;
  struct tmpfs_inode_info *reclaim_node = NULL;
  int reclaim_last = 0, reclaim_is_dir = 0;

  // 1. Detach the old_name node from old_dir
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

  // Directory boundary checks (matching Linux rename(2)):
  // - old is a directory, new exists and is non-empty → -ENOTEMPTY
  // - old is a directory, new exists and is not a directory → -EISDIR
  // - old is not a directory, new exists and is a directory → -ENOTDIR
  // - old is an ancestor of new / new is an ancestor of old (cycle) → -EINVAL
  // The db scenario has both as regular files, so these branches never trigger.
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
        rc = -ENOTEMPTY; // new directory is non-empty
      // Cycle detection: walk new's parent chain to confirm old is not in it
      // (tmpfs_inode_info.parent is a back-pointer, O(depth)). Never triggers
      // in the db scenario.
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
      // Rollback: re-insert into old_dir
      node->sibling = old_ti->children;
      old_ti->children = node;
      spin_unlock(&second_ti->lock);
      if (first_ti != second_ti)
        spin_unlock(&first_ti->lock);
      spin_unlock_irqrestore(&tmpfs_rename_lock, rflags);
      return rc;
    }
    // Overwrite semantics: detach the exist node from new_ti, commit the
    // directory entry; inode/data reclamation is deferred until after unlocking
    // (matching tmpfs_unlink:287-302; an already-open fd keeps the inode alive
    // via i_count>1).
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

  // 2. Rename + head-insert into the new_dir children chain
  int i = 0;
  while (new_name[i] && i < 255) {
    node->name[i] = new_name[i];
    i++;
  }
  node->name[i] = '\0';
  node->parent = new_ti;
  node->sibling = new_ti->children;
  new_ti->children = node;

  // 3. Release all locks — the directory entry is committed, the overwritten
  // inode is no longer reachable via any directory
  spin_unlock(&second_ti->lock);
  if (first_ti != second_ti)
    spin_unlock(&first_ti->lock);
  spin_unlock_irqrestore(&tmpfs_rename_lock, rflags);

  // 4. Reclaim the overwritten node after unlocking (matching tmpfs_unlink's
  // reclamation discipline; inode_put takes inode_hash_lock +
  // page_cache_invalidate + kfree, so it cannot run under ti->lock).
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
      // §3.4 nlink maintenance: rmdir of a subdirectory decrements the parent's
      // nlink (matching mkdir's ++). Adjusted for the parent before ip is
      // released; ip itself is reclaimed via inode_put (nlink no longer read).
      mutex_lock(&dir->i_lock);
      dir->nlink--;
      mutex_unlock(&dir->i_lock);
      // IN_DELETE on the parent (name) + IN_DELETE_SELF on the removed dir.
      inotify_inode_event(dir, IN_DELETE, 0, name);
      inotify_inode_event(ip, IN_DELETE_SELF, 0, NULL);
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

// Emit the synthetic "." and ".." entries at the head of a directory listing.
// Like a real child, each honors the resume cursor (skip while cur_pos <
// ctx->pos) and stops on a buffer-full dir_emit. "." is the directory itself;
// ".." is the parent (the tmpfs root points to itself, matching Linux). Returns
// false if the buffer filled before the requested entry could be emitted.
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

// ===== getdents (fstype callback) =====
static ssize_t tmpfs_getdents(struct inode *dir, struct dir_context *ctx) {
  struct tmpfs_inode_info *ti = (struct tmpfs_inode_info *)dir->i_priv;
  if (!ti)
    return 0;
  spin_lock(&ti->lock);
  // EOF marker from previous call
  if (ctx->pos == (uint64_t)-1) {
    spin_unlock(&ti->lock);
    return 0;
  }
  size_t cur_pos = 0;
  // Synthetic "." and ".." precede real children (POSIX/Linux convention for
  // in-memory filesystems, which have no on-disk dot entries).
  uint64_t parent_ino =
      ti->parent ? ti->parent->inode->ino : dir->ino; // root: .. → self
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
  ctx->pos = (uint64_t)-1; // EOF: all entries emitted
done:
  spin_unlock(&ti->lock);
  return (ssize_t)ctx->written;
}

// ===== mount_root =====
static struct inode *tmpfs_mount_root(struct mount_entry *m) {
  if (!m->root) {
    uint32_t ino = tmpfs_alloc_ino();
    struct inode *root = inode_create(&m->sb, ino, INODE_DIR, 0);
    if (!root)
      return NULL;
    root->i_op = &tmpfs_dir_iop;
    root->i_priv = new_tmpfs_info(root, NULL);
    if (!root->i_priv) {
      inode_put(root);
      return NULL;
    }
    root->mount = m;
    m->root = root;
  }
  return inode_get(m->root);
}

// ===== fops: read/write =====
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
  if (tmpfs_copy_out_locked(ip, off, buf, n, true) != 0) {
    spin_unlock(&ti->lock);
    return -EFAULT;
  }
  f->offset = off + n;
  spin_unlock(&ti->lock);
  return (ssize_t)n;
}

static ssize_t tmpfs_read_at(struct xtask *proc, struct file *f, void *buf,
                             size_t count, uint64_t offset) {
  struct inode *ip = f->inode;
  struct tmpfs_inode_info *ti = (struct tmpfs_inode_info *)ip->i_priv;
  if (!ti)
    return -EFAULT;
  spin_lock(&ti->lock);
  if (offset >= ti->size) {
    spin_unlock(&ti->lock);
    return 0;
  }
  size_t n = ti->size - offset < count ? ti->size - offset : count;
  // vfs_read_kernel passes proc == NULL and a kernel destination.
  if (tmpfs_copy_out_locked(ip, offset, buf, n, proc != NULL) != 0) {
    spin_unlock(&ti->lock);
    return -EFAULT;
  }
  spin_unlock(&ti->lock);
  return (ssize_t)n;
}

// tmpfs_read_kern: kernel-mode read of a tmpfs regular file (execve calls it
// via vfs_read_kernel). Differs from tmpfs_read in using an explicit offset
// (not f->offset) and __memcpy to a kernel buf (not copy_to_user). Signature
// matches fat32_read so vfs_read_kernel can dispatch by fstype.
int tmpfs_read_kern(struct inode *ip, uint64_t offset, void *buf,
                    size_t count) {
  struct tmpfs_inode_info *ti = (struct tmpfs_inode_info *)ip->i_priv;
  if (!ti)
    return -EFAULT;
  spin_lock(&ti->lock);
  if (offset >= ti->size) {
    spin_unlock(&ti->lock);
    return 0;
  }
  size_t n = ti->size - offset < count ? ti->size - offset : count;
  int rc = tmpfs_copy_out_locked(ip, offset, buf, n, false);
  spin_unlock(&ti->lock);
  return rc == 0 ? (ssize_t)n : (ssize_t)rc;
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
  int rc = tmpfs_grow_locked(ip, ti, need);
  if (rc != 0) {
    spin_unlock(&ti->lock);
    return rc;
  }
  if (off > ti->size)
    tmpfs_zero_locked(ip, ti->size, off - ti->size);
  if (tmpfs_copy_in_locked(ip, off, buf, count) != 0) {
    spin_unlock(&ti->lock);
    return -EFAULT;
  }
  if (need > ti->size)
    ti->size = need;
  ip->size = ti->size;
  f->offset = off + count;
  spin_unlock(&ti->lock);
  // IN_MODIFY on the written file (ti->lock released above).
  inotify_inode_event(ip, IN_MODIFY, 0, NULL);
  return (ssize_t)count;
}

const struct file_operations tmpfs_file_fops = {
    .read = tmpfs_read,
    .read_at = tmpfs_read_at,
    .write = tmpfs_write,
};

// ===== fstype =====
struct fstype tmpfs_fstype = {
    .name = "tmpfs",
    .mount_root = tmpfs_mount_root,
    .getdents = tmpfs_getdents,
};

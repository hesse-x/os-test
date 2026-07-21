/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/bsd/mount.h"
#include "arch/x64/utils.h"
#include "kernel/bsd/inode.h"
#include "kernel/xcore/mem/kasan.h"
#include "kernel/xcore/spinlock.h"
#include <xos/dirent.h>
#include <xos/errno.h>

#define MAX_FSTYPES 8
static struct fstype *fstype_table[MAX_FSTYPES];
static int fstype_count = 0;

static struct mount_entry mount_table[MAX_MOUNTS];
static spinlock mount_lock = SPINLOCK_INIT;

void mount_init(void) {
  for (int i = 0; i < MAX_MOUNTS; i++) {
    mount_table[i].in_use = false;
    mount_table[i].mntpoint[0] = '\0';
    mount_table[i].fs = NULL;
    mount_table[i].fs_data = NULL;
  }
  fstype_count = 0;
}

void register_fstype(struct fstype *fs) {
  if (fstype_count < MAX_FSTYPES)
    fstype_table[fstype_count++] = fs;
}

struct fstype *find_fstype_by_name(const char *name) {
  for (int i = 0; i < fstype_count; i++) {
    if (__strcmp(name, fstype_table[i]->name) == 0)
      return fstype_table[i];
  }
  return NULL;
}

int mount_internal(struct fstype *fs, const char *target, void *fs_data) {
  if (target[0] != '/')
    return -EINVAL;
  spin_lock(&mount_lock);
  for (int i = 0; i < MAX_MOUNTS; i++) {
    if (mount_table[i].in_use &&
        __strcmp(mount_table[i].mntpoint, target) == 0) {
      spin_unlock(&mount_lock);
      return -EBUSY;
    }
  }
  int slot = -1;
  for (int i = 0; i < MAX_MOUNTS; i++) {
    if (!mount_table[i].in_use) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    spin_unlock(&mount_lock);
    return -ENOMEM;
  }
  int j;
  for (j = 0; target[j] && j < MNTPOINT_MAX - 1; j++)
    mount_table[slot].mntpoint[j] = target[j];
  mount_table[slot].mntpoint[j] = '\0';
  mount_table[slot].fs = fs;
  mount_table[slot].fs_data = fs_data;
  mount_table[slot].in_use = true;
  spin_unlock(&mount_lock);
  return 0;
}

struct mount_entry *vfs_resolve(const char *path, char *relpath,
                                size_t relcap) {
  spin_lock(&mount_lock);
  struct mount_entry *best = NULL;
  size_t best_len = 0;
  for (int i = 0; i < MAX_MOUNTS; i++) {
    if (!mount_table[i].in_use)
      continue;
    const char *mp = mount_table[i].mntpoint;
    size_t mplen = 0;
    while (mp[mplen])
      mplen++;
    if (mplen == 1 && mp[0] == '/') {
      /* root mount matches any absolute path */
      if (mplen > best_len) {
        best = &mount_table[i];
        best_len = mplen;
      }
      continue;
    }
    if (path[mplen] == '/' || path[mplen] == '\0') {
      int match = 1;
      for (size_t k = 0; k < mplen; k++) {
        if (path[k] != mp[k]) {
          match = 0;
          break;
        }
      }
      if (match && mplen > best_len) {
        best = &mount_table[i];
        best_len = mplen;
      }
    }
  }
  if (best) {
    const char *rest = path + best_len;
    while (*rest == '/')
      rest++;
    size_t r = 0;
    while (rest[r] && r < relcap - 1) {
      relpath[r] = rest[r];
      r++;
    }
    relpath[r] = '\0';
  }
  spin_unlock(&mount_lock);
  return best;
}

struct mount_entry *vfs_resolve_user(const char __user *upath, char *relpath,
                                     size_t relcap) {
  char kpath[256];
  char norm[256];
  if (strncpy_from_user(kpath, upath, sizeof(kpath)) < 0)
    return ERR_PTR(-EFAULT);
  if (kpath[0] != '/')
    return ERR_PTR(-EINVAL);
  if (normalize_path(kpath, norm, sizeof(norm)) < 0)
    return ERR_PTR(-ENAMETOOLONG);
  return vfs_resolve(norm, relpath, relcap);
}

struct mount_entry *mount_of_inode(struct inode *ip) {
  if (ip && ip->mount)
    return ip->mount;
  /* Fallback: find root mount "/" */
  spin_lock(&mount_lock);
  for (int i = 0; i < MAX_MOUNTS; i++) {
    if (mount_table[i].in_use && mount_table[i].mntpoint[0] == '/' &&
        mount_table[i].mntpoint[1] == '\0') {
      spin_unlock(&mount_lock);
      return &mount_table[i];
    }
  }
  spin_unlock(&mount_lock);
  return NULL;
}

bool dir_emit(struct dir_context *ctx, const char *name, int namlen,
              uint64_t offset, uint64_t ino, unsigned int d_type) {
  /* dirent64 layout: d_ino(8) + d_off(8) + d_reclen(2) + d_type(1) +
   * d_name[namlen+1], padded to 8-byte alignment. */
  uint16_t reclen = (uint16_t)(sizeof(struct dirent64) + namlen + 1);
  reclen = (reclen + 7) & ~7;
  if (ctx->written + reclen > ctx->len)
    return false;
  struct dirent64 *d = (struct dirent64 *)((uint8_t *)ctx->buf + ctx->written);
  d->d_ino = ino;
  d->d_off = offset;
  d->d_reclen = reclen;
  d->d_type = (uint8_t)d_type;
  for (int i = 0; i < namlen; i++)
    d->d_name[i] = name[i];
  d->d_name[namlen] = '\0';
  ctx->written += reclen;
  ctx->pos = offset + reclen; /* resumption point: next entry */
  return true;
}

int normalize_path(const char *in, char *out, size_t outcap) {
  /* String-level normalization: split by '/', skip '.', pop on '..'.
   * Clamp '..' at root (cannot go above '/'). No symlink handling. */
  if (in[0] != '/')
    return -EINVAL;
  /* Stack of component start offsets in 'out' (in-place build). */
  int stack[64];
  int sp = 0;
  size_t outpos = 0;
  out[outpos++] = '/';
  size_t i = 1;
  while (in[i]) {
    /* skip leading slashes */
    while (in[i] == '/')
      i++;
    if (!in[i])
      break;
    size_t start = i;
    while (in[i] && in[i] != '/')
      i++;
    size_t complen = i - start;
    if (complen == 1 && in[start] == '.') {
      /* skip */
    } else if (complen == 2 && in[start] == '.' && in[start + 1] == '.') {
      /* pop */
      if (sp > 0) {
        sp--;
        outpos = (sp == 0) ? 1 : stack[sp];
      }
    } else {
      if (outpos > 1)
        out[outpos++] = '/';
      if (outpos + complen + 1 > outcap)
        return -ENAMETOOLONG;
      stack[sp++] = outpos;
      for (size_t k = 0; k < complen; k++)
        out[outpos++] = in[start + k];
    }
  }
  if (outpos == 0)
    out[outpos++] = '/';
  out[outpos] = '\0';
  return 0;
}

/* SYS_MOUNT(source, target, fstype, flags, data) — Linux 5-param signature.
 * source/flags/data currently reserved (passed 0/NULL). No permission
 * check: this OS has no user/capability model. */
int64_t sys_mount(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4,
                  int64_t arg5, int64_t unused) {
  (void)arg4;
  (void)arg5;
  (void)unused;
  const char __user *utarget = (const char __user *__force)arg2;
  const char __user *utype = (const char __user *__force)arg3;

  if (!utarget || !utype)
    return -EFAULT;

  char type[16];
  char target[64];
  if (strncpy_from_user(type, utype, sizeof(type)) < 0)
    return -EFAULT;
  type[15] = '\0';
  if (strncpy_from_user(target, utarget, sizeof(target)) < 0)
    return -EFAULT;
  target[63] = '\0';

  if (target[0] != '/')
    return -EINVAL;

  struct fstype *fs = find_fstype_by_name(type);
  if (!fs)
    return -ENODEV;

  return mount_internal(fs, target, NULL);
}

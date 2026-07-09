/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/bsd/devtmpfs.h"
#include "arch/x64/utils.h"
#include "kernel/bsd/inode.h"
#include "kernel/bsd/netlink.h"
#include "kernel/bsd/proc.h"
#include "kernel/bsd/types.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/trap.h"
#include "kernel/xcore/xtask.h"
#include <stddef.h>
#include <xos/errno.h>

struct shm;

#define MAX_DEV_ENTRIES 32

struct dev_entry {
  char name[32];
  struct inode *ip;
  struct dev_entry *next;
};

/* Directory entry for subdirectory support (e.g. "dri") */
struct dev_dir {
  char name[32];
  struct inode *ip; /* INODE_DIR inode */
  struct dev_dir *next;
};

static struct dev_dir dev_dirs[16]; /* max 16 subdirectories */
static struct dev_dir *dir_list = NULL;
static int dir_count = 0;

static struct dev_entry dev_entries[MAX_DEV_ENTRIES];
static struct dev_entry *dev_list = NULL;
static int dev_count = 0;
static spinlock devtmpfs_lock = SPINLOCK_INIT;

static bool devtmpfs_initialized = false;

void devtmpfs_init(void) {
  if (devtmpfs_initialized) {
    printk(LOG_INFO, "devtmpfs_init: already initialized, skip\n");
    return;
  }
  spin_lock(&devtmpfs_lock);
  dev_list = NULL;
  dev_count = 0;
  dir_list = NULL;
  dir_count = 0;
  for (int i = 0; i < MAX_DEV_ENTRIES; i++) {
    dev_entries[i].name[0] = '\0';
    dev_entries[i].ip = NULL;
    dev_entries[i].next = NULL;
  }
  for (int i = 0; i < 16; i++) {
    dev_dirs[i].name[0] = '\0';
    dev_dirs[i].ip = NULL;
    dev_dirs[i].next = NULL;
  }
  spin_unlock(&devtmpfs_lock);
  devtmpfs_initialized = true;
  printk(LOG_INFO, "devtmpfs_init: done\n");
}

/* Find or create a subdirectory dev_dir entry by name (no slash in name) */
static struct dev_dir *devtmpfs_find_dir(const char *name) {
  for (struct dev_dir *d = dir_list; d; d = d->next) {
    if (__strcmp(name, d->name) == 0)
      return d;
  }
  return NULL;
}

static struct dev_dir *devtmpfs_get_or_create_dir(const char *name, int len) {
  char tmp[32];
  if (len >= 31)
    return NULL;
  for (int i = 0; i < len; i++)
    tmp[i] = name[i];
  tmp[len] = '\0';
  struct dev_dir *d = devtmpfs_find_dir(tmp);
  if (d)
    return d;
  if (dir_count >= 16)
    return NULL;
  /* find free slot */
  for (int i = 0; i < 16; i++) {
    if (dev_dirs[i].ip == NULL) {
      struct inode *ip = inode_create(0, INODE_DIR, 0, 0, 0, 0);
      if (!ip)
        return NULL;
      for (int j = 0; j < len; j++)
        dev_dirs[i].name[j] = tmp[j];
      dev_dirs[i].name[len] = '\0';
      dev_dirs[i].ip = ip;
      dev_dirs[i].next = dir_list;
      dir_list = &dev_dirs[i];
      dir_count++;
      return &dev_dirs[i];
    }
  }
  return NULL;
}

struct inode *devtmpfs_lookup(const char *name) {
  /* Strip "/dev/" prefix — stored entries use paths without this prefix */
  if (__memcmp(name, "/dev/", 5) == 0)
    name += 5;
  /* If path contains '/', split into dir + leaf */
  const char *slash = name;
  while (*slash && *slash != '/')
    slash++;
  if (*slash == '/') {
    int dir_len = slash - name;
    char dir_name[32];
    if (dir_len >= 31)
      return NULL;
    for (int i = 0; i < dir_len; i++)
      dir_name[i] = name[i];
    dir_name[dir_len] = '\0';
    spin_lock(&devtmpfs_lock);
    struct dev_dir *d = devtmpfs_find_dir(dir_name);
    spin_unlock(&devtmpfs_lock);
    if (!d)
      return NULL;
    /* lookup leaf inside dir: match by full path (stored entry.name includes
     * dir/ prefix) */
    spin_lock(&devtmpfs_lock);
    struct dev_entry *e = dev_list;
    while (e) {
      if (__strcmp(name, e->name) == 0) {
        spin_unlock(&devtmpfs_lock);
        return e->ip;
      }
      e = e->next;
    }
    spin_unlock(&devtmpfs_lock);
    return NULL;
  }
  /* No slash: flat lookup */
  spin_lock(&devtmpfs_lock);
  struct dev_entry *e = dev_list;
  while (e) {
    if (__strcmp(name, e->name) == 0) {
      spin_unlock(&devtmpfs_lock);
      return e->ip;
    }
    e = e->next;
  }
  spin_unlock(&devtmpfs_lock);
  return NULL;
}

int devtmpfs_create(const char *name, struct dev_ops *ops, struct shm *shm) {
  WARN_ON(!devtmpfs_initialized); // catch order bugs: create before init
  if (dev_count >= MAX_DEV_ENTRIES)
    return -ENOMEM;

  /* Check if already exists (full path) */
  if (devtmpfs_lookup(name))
    return -EEXIST;

  /* If path contains '/', create the subdirectory first */
  const char *slash = name;
  while (*slash && *slash != '/')
    slash++;
  if (*slash == '/') {
    int dir_len = slash - name;
    spin_lock(&devtmpfs_lock);
    devtmpfs_get_or_create_dir(name, dir_len);
    spin_unlock(&devtmpfs_lock);
  }

  spin_lock(&devtmpfs_lock);

  /* Find free slot */
  int slot = -1;
  for (int i = 0; i < MAX_DEV_ENTRIES; i++) {
    if (dev_entries[i].ip == NULL) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    spin_unlock(&devtmpfs_lock);
    return -ENOMEM;
  }

  /* Create inode */
  struct inode *ip = inode_create(0, INODE_DEV, 0, 0, 0, 0);
  if (!ip) {
    spin_unlock(&devtmpfs_lock);
    return -ENOMEM;
  }
  ip->i_priv = ops;
  if (shm) {
    shm_get(shm); // +1 for inode reference
    ip->shm = shm;
  } else {
    ip->shm = NULL;
  }

  /* Fill entry — store full path including any '/' */
  int i;
  for (i = 0; name[i] && i < 31; i++)
    dev_entries[slot].name[i] = name[i];
  dev_entries[slot].name[i] = '\0';
  dev_entries[slot].ip = ip;
  dev_entries[slot].next = dev_list;
  dev_list = &dev_entries[slot];
  dev_count++;

  spin_unlock(&devtmpfs_lock);
  printk(LOG_INFO, "devtmpfs: created /dev/%s\n", name);

  // Broadcast uevent if netlink initialized
  if (devtmpfs_initialized && nl_is_initialized()) {
    const char *subsys = (ops && ops->is_block) ? "block" : "input";
    nl_uevent_broadcast("add", name, subsys);
  }
  return 0;
}

uint64_t devtmpfs_open(xtask *proc, const char *name, int flags) {
  struct inode *ip = devtmpfs_lookup(name);
  if (!ip)
    return (uint64_t)(-(uint64_t)ENOENT);

  /* Allocate fd (under fd_lock) */
  spinlock *fdlk = &proc->proc->files->fd_lock;
  spin_lock(fdlk);
  int fd = alloc_fd(proc->proc->files, 3);
  if (fd < 0) {
    spin_unlock(fdlk);
    return (uint64_t)(-(uint64_t)EMFILE);
  }

  struct file *f = kmalloc(sizeof(struct file));
  if (!f) {
    spin_unlock(fdlk);
    return (uint64_t)(-(uint64_t)ENOMEM);
  }
  __memset(f, 0, sizeof(*f));
  refcount_set(&f->f_count, 1);
  f->type = FD_DEV;
  f->flags = flags;
  f->inode = ip;
  inode_get(ip);

  // Install FD_DEV BEFORE ops->open so callbacks (e.g. pts_open/ptmx_open)
  // can access it via fd_table[fd] and mutate it into FD_TTY in place.
  fd_install(proc->proc->files, fd, f);

  if (ip->i_priv) {
    struct dev_ops *ops = (struct dev_ops *)ip->i_priv;
    f->target_pid = ops->driver_pid;
    // Kernel device: call open callback. Callbacks mutate the FD_DEV file
    // in place (do not replace the pointer), so fd_table[fd] stays valid.
    if (ops->driver_pid == 0 && ops->open) {
      int rc = ops->open(proc, fd);
      if (rc < 0) {
        // Open failed: undo fd installation.
        // Manual cleanup (not file_put) to avoid calling ops->close
        // when ops->open itself failed.
        fd_uninstall(proc->proc->files, fd);
        inode_put(ip);
        kfree(f);
        spin_unlock(fdlk);
        return (uint64_t)(-(uint64_t)(-rc));
      }
    }
  }
  spin_unlock(fdlk);
  return (uint64_t)fd;
}

void devtmpfs_cleanup_pid(pid_t pid) {
  spin_lock(&devtmpfs_lock);
  struct dev_entry **pp = &dev_list;
  while (*pp) {
    struct dev_entry *e = *pp;
    if (e->ip && e->ip->i_priv) {
      struct dev_ops *ops = (struct dev_ops *)e->ip->i_priv;
      if (ops->driver_pid == pid) {
        /* Remove from list */
        *pp = e->next;
        /* Free kmalloc'd dev_ops (user-space driver) */
        if (ops->driver_pid > 0)
          kfree(ops);
        /* Free inode */
        inode_put(e->ip);
        e->ip = NULL;
        e->name[0] = '\0';
        dev_count--;
        continue;
      }
    }
    pp = &e->next;
  }
  spin_unlock(&devtmpfs_lock);
}

void devtmpfs_remove(const char *name) {
  bool removed = false;
  spin_lock(&devtmpfs_lock);
  struct dev_entry **pp = &dev_list;
  while (*pp) {
    struct dev_entry *e = *pp;
    if (__strcmp(name, e->name) == 0) {
      *pp = e->next;
      if (e->ip)
        inode_put(e->ip);
      e->ip = NULL;
      e->name[0] = '\0';
      dev_count--;
      removed = true;
      break;
    }
    pp = &e->next;
  }
  spin_unlock(&devtmpfs_lock);

  if (removed && devtmpfs_initialized && nl_is_initialized())
    nl_uevent_broadcast("remove", name, "misc");
}

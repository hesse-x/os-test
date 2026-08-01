/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef KERNEL_BSD_SYSFS_H
#define KERNEL_BSD_SYSFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kernel/bsd/fops.h"
#include "kernel/bsd/mount.h"

struct inode;
struct kstat;

// sysfs attribute callback (mirrors Linux kobject_attribute).
struct sysfs_attr {
  const char *name;
  void *priv;                                         // device context
  ssize_t (*show)(char *buf, size_t len, void *priv); // read
  ssize_t (*store)(const char *buf, size_t len,
                   void *priv); // write (default NULL)
};

// uevent attribute priv: devpath + subsystem needed for re-broadcast.
// The sysfs node basename strips the subsystem prefix (sysfs.c basename parse),
// and a file node's inode->i_priv holds only the attr (not the sysfs_node), so
// the store callback cannot reconstruct the full devpath from node position —
// store it here. Lifetime managed by dev_ops.uevent_priv (kfree'd before
// sysfs_remove_dir).
struct uevent_attr_priv {
  char devpath[32];  // e.g. "input/event0" (= devtmpfs name, the DEVPATH value)
  char subsystem[8]; // e.g. "input"
};

// evdev device attributes (kernel side).
struct input_dev_props {
  uint16_t bustype;
  uint16_t vendor;
  uint16_t product;
  uint16_t version;
  char name[64];
};

// sysfs node (directory or attribute file).
struct sysfs_node {
  char name[32];
  bool is_dir;
  bool attr_owned; // true = attr is kmalloc'd, free on removal
  struct sysfs_node *parent;
  struct sysfs_node *children;
  struct sysfs_node *sibling;
  struct sysfs_attr *attr; // file: attribute; directory: NULL
  struct inode *ip;        // associated inode (created on lookup)
  uint32_t ino;            // unique inode number
};

// evdev attributes (referenced by devtmpfs, defined in sysfs.c).
extern const struct sysfs_attr evdev_attr_name;
extern const struct sysfs_attr evdev_attr_bustype;
extern const struct sysfs_attr evdev_attr_vendor;
extern const struct sysfs_attr evdev_attr_product;
extern const struct sysfs_attr evdev_attr_version;
// Writable uevent attribute template (coldplug writes "add" to re-broadcast);
// devtmpfs copies per device and fills priv.
extern const struct sysfs_attr uevent_attr;

// API
struct sysfs_node *sysfs_create_dir(struct sysfs_node *parent,
                                    const char *name);
struct sysfs_node *sysfs_create_file(struct sysfs_node *parent,
                                     const char *name,
                                     const struct sysfs_attr *attr);
void sysfs_remove_dir(struct sysfs_node *dir);
struct sysfs_node *sysfs_class_dir(const char *subsystem);
void sysfs_init(void);
struct sysfs_node *sysfs_root_node(void);

// fstype callbacks
struct inode *sysfs_lookup(const char *relpath);
ssize_t sysfs_getdents(struct inode *dir, struct dir_context *ctx);
int sysfs_stat(const char *relpath, struct kstat *ks);

extern struct fstype sysfs_fstype;
extern const struct file_operations sysfs_fops;

#endif

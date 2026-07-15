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

struct file;
struct inode;
struct kstat;

/* sysfs 属性回调 (对齐 Linux kobject_attribute) */
struct sysfs_attr {
  const char *name;
  void *priv;                                         /* 设备上下文 */
  ssize_t (*show)(char *buf, size_t len, void *priv); /* 读 */
  ssize_t (*store)(const char *buf, size_t len, void *priv); /* 写(默认 NULL) */
};

/* uevent 属性的 priv:重广播所需的 devpath + subsystem。
 * sysfs 节点 basename 去掉了子系统前缀(sysfs.c basename 解析),
 * 且文件节点 inode->i_priv 只存 attr 不存 sysfs_node,store 回调
 * 无法从节点位置反推完整 devpath,故单独存。生命周期由 dev_ops.uevent_priv
 * 管理(在 sysfs_remove_dir 前 kfree)。 */
struct uevent_attr_priv {
  char devpath[32];  /* 如 "input/event0"(= devtmpfs name,DEVPATH 值) */
  char subsystem[8]; /* 如 "input" */
};

/* evdev 设备属性 (内核侧) */
struct input_dev_props {
  uint16_t bustype;
  uint16_t vendor;
  uint16_t product;
  uint16_t version;
  char name[64];
};

/* sysfs 节点 (目录或属性文件) */
struct sysfs_node {
  char name[32];
  bool is_dir;
  bool attr_owned; /* true = attr is kmalloc'd, free on removal */
  struct sysfs_node *parent;
  struct sysfs_node *children;
  struct sysfs_node *sibling;
  struct sysfs_attr *attr; /* 文件: 属性; 目录: NULL */
  struct inode *ip;        /* 关联 inode (lookup 时按需建) */
  uint32_t ino;            /* 唯一 inode 号 */
};

/* evdev 属性 (由 devtmpfs 引用, 在 sysfs.c 定义) */
extern const struct sysfs_attr evdev_attr_name;
extern const struct sysfs_attr evdev_attr_bustype;
extern const struct sysfs_attr evdev_attr_vendor;
extern const struct sysfs_attr evdev_attr_product;
extern const struct sysfs_attr evdev_attr_version;
/* 可写 uevent 属性模板(coldplug 写 "add" 重广播);devtmpfs 逐设备拷贝填 priv */
extern const struct sysfs_attr uevent_attr;

/* API */
struct sysfs_node *sysfs_create_dir(struct sysfs_node *parent,
                                    const char *name);
struct sysfs_node *sysfs_create_file(struct sysfs_node *parent,
                                     const char *name,
                                     const struct sysfs_attr *attr);
void sysfs_remove_dir(struct sysfs_node *dir);
struct sysfs_node *sysfs_class_dir(const char *subsystem);
void sysfs_init(void);
struct sysfs_node *sysfs_root_node(void);

/* fstype 回调 */
struct inode *sysfs_lookup(const char *relpath);
ssize_t sysfs_getdents(struct inode *dir, struct dir_context *ctx);
int sysfs_stat(const char *relpath, struct kstat *ks);

extern struct fstype sysfs_fstype;
extern const struct file_operations sysfs_fops;
extern const struct file_operations ringbuf_fops;

/* ringbuf lifecycle notification: send RINGBUF_OPEN to driver_pid */
void ringbuf_notify_open(struct inode *ip, int32_t opener_pid);

/* ringbuf cursor: initialize consumer offset to current head on open */
void ringbuf_init_cursor(struct inode *ip, struct file *f);

#endif

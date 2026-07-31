/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef KERNEL_BSD_PROCFS_H
#define KERNEL_BSD_PROCFS_H

#include <stdbool.h>
#include <stdint.h>

#include "kernel/bsd/fops.h"
#include "kernel/bsd/mount.h"

#include <xos/types.h> // pid_t

/* procfs 属性回调（procfs.md §3.1）。只读：无 store。pid 由 inode->ino 反解。
 */
struct procfs_attr {
  const char *name;
  ssize_t (*show)(char *buf, size_t len, pid_t pid);
};

enum procfs_node_kind {
  PROCFS_STATIC,  /* mount 时预建的全局节点(meminfo/cpuinfo/...) */
  PROCFS_PIDDIR,  /* /proc/[pid] 目录:lookup 时合成 */
  PROCFS_PIDATTR, /* /proc/[pid]/xxx 文件:show 时从 tasks[pid] 读 */
  PROCFS_PIDFD,   /* /proc/[pid]/fd/N 符号链接:readlink 时合成 */
  PROCFS_MAGIC,   /* /proc/self 魔幻链接 */
};

struct procfs_node {
  char name[32]; /* 对齐 sysfs_node.name[32] (sysfs.h:48) */
  enum procfs_node_kind kind;
  bool is_dir;
  struct procfs_node *parent, *children, *sibling; /* 内存树,仿 sysfs */
  struct procfs_attr *attr; /* 文件:属性;目录:NULL */
  struct inode *ip;         /* 关联 inode,lookup 时按需建 */
  uint32_t ino;
};

/* fstype 注册入口(供 vfs_init 调用) */
extern struct fstype procfs_fstype;
extern const struct file_operations procfs_fops;
struct procfs_node *procfs_root_node(void); /* 返回 fs_data */
void procfs_init(void);                     /* 预建全局静态节点 */

/* 进程生命周期 hook(供 proc.c 调用) */
void procfs_pinfo_set(pid_t pid, const char *exe, char *const argv[],
                      char *const envp[]);
void procfs_pinfo_clear(pid_t pid);

/* inode_operations 为 procfs.c 内部 static(file-private),不在此导出,
 * 与 sysfs.c 的 sysfs_dir_iop/sysfs_file_iop 同款。 */

#endif // KERNEL_BSD_PROCFS_H

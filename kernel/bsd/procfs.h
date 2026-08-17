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

// procfs attribute callback (procfs.md §3.1). Read-only: no store. pid is
// decoded from inode->ino.
struct procfs_attr {
  const char *name;
  ssize_t (*show)(char *buf, size_t len, pid_t pid);
};

enum procfs_node_kind {
  PROCFS_STATIC,  // global node pre-built at mount (meminfo/cpuinfo/...)
  PROCFS_PIDDIR,  // /proc/[pid] directory: synthesized at lookup
  PROCFS_PIDATTR, // /proc/[pid]/xxx file: read from tasks[pid] at show
  PROCFS_PIDFD,   // /proc/[pid]/fd/N symlink: synthesized at readlink
  PROCFS_MAGIC,   // /proc/self magic link
};

struct procfs_node {
  char name[32]; // matches sysfs_node.name[32] (sysfs.h:48)
  enum procfs_node_kind kind;
  bool is_dir;
  struct procfs_node *parent, *children, *sibling; // in-memory tree, like sysfs
  struct procfs_attr *attr;                        // file: attribute; dir: NULL
  struct inode *ip; // associated inode, created on demand at lookup
  uint32_t ino;
};

// fstype registration entry (for vfs_init to call)
extern struct fstype procfs_fstype;
extern const struct file_operations procfs_fops;
struct procfs_node *procfs_root_node(void); // returns fs_data
void procfs_init(void);                     // pre-build the global static nodes

// Process lifecycle hooks (for proc.c to call)
void procfs_pinfo_set(pid_t pid, const char *exe, char *const argv[],
                      char *const envp[]);
void procfs_pinfo_clear(pid_t pid);

// The inode_operations are static (file-private) inside procfs.c, not exported
// here, same as sysfs.c's sysfs_dir_iop/sysfs_file_iop.

#endif // KERNEL_BSD_PROCFS_H

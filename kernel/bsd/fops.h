/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef KERNEL_BSD_FOPS_H
#define KERNEL_BSD_FOPS_H

#include <stddef.h>
#include <stdint.h>

typedef int64_t ssize_t;
typedef uint32_t __poll;

struct file;
struct xtask;

struct file_operations {
  ssize_t (*read)(struct xtask *proc, struct file *f, void *buf, size_t count);
  ssize_t (*write)(struct xtask *proc, struct file *f, const void *buf,
                   size_t count);
  long (*ioctl)(struct xtask *proc, struct file *f, uint32_t cmd, void *arg);
  __poll (*poll)(struct xtask *proc, struct file *f, int events);
  int (*close)(struct xtask *proc, struct file *f);
  uint64_t (*mmap)(struct xtask *proc, struct file *f, uint64_t size);
};

#endif

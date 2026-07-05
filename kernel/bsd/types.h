/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_BSD_TYPES_H
#define KERNEL_BSD_TYPES_H

#include "kernel/xcore/atomic.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/mm_types.h" // mm_t, mmap_region_t, shm_t
#include "kernel/xcore/rcu.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/xtask.h" // pid_t, xtask_t
#include <stdint.h>
#include <xos/fcntl.h>
#include <xos/mman.h>
#include <xos/types.h>

// ===================== fd / pipe =====================
#define MAX_FD 32
#define PIPE_BUF_SIZE 4096
#define FD_CLOEXEC 0x8000

#define FD_NONE 0
#define FD_PIPE 1
#define FD_REGULAR 2
#define FD_DEV 3
#define FD_DIR 4
#define FD_SOCKET 5
#define FD_SHM 6
#define FD_FILE 7
#define FD_TTY 8

typedef struct pipe {
  uint8_t *buf;
  uint32_t head;
  uint32_t tail;
  pid_t read_pid;
  pid_t write_pid;
  refcount_t p_count;
} pipe_t;

struct unix_sock;
struct inode;
struct pty;

typedef struct file {
  refcount_t f_count;
  int type;
  int flags;
  struct inode *inode;
  uint64_t offset;
  union {
    struct pipe *pipe;
    struct shm *shm;
    pid_t target_pid;
    struct {
      pid_t fs_pid;
      int32_t fs_fd;
      uint64_t _offset;
      uint64_t file_size;
      refcount_t f_count;
    } file_data;
    struct unix_sock *sock;
    struct pty *pty;
  };
} file_t;

typedef struct files_t {
  spinlock_t fd_lock;
  struct file *fd_table[MAX_FD];
  refcount_t f_count;
} files_t;

// mm_t, mmap_region_t, shm_t defined in kernel/xcore/mm_types.h
// shm_get/shm_put declared in kernel/xcore/trap.h
// copy_page_table/add_mmap_region declared in kernel/xcore/kpi.h

// ===================== files_t lifecycle =====================
files_t *files_create(void);
void files_put(files_t *files);

// ===================== unified fd lifecycle =====================
void file_put(struct file *f);
int alloc_fd(files_t *files, int min_fd);
void pty_dup_file(struct file *f);
void pty_close_file(struct file *f);

static inline void file_get(struct file *f) {
  if (f)
    refcount_inc(&f->f_count);
}

static inline void fd_install(files_t *files, int fd, struct file *f) {
  rcu_assign_pointer(files->fd_table[fd], f);
}

static inline struct file *fd_uninstall(files_t *files, int fd) {
  struct file *f = files->fd_table[fd];
  rcu_assign_pointer(files->fd_table[fd], NULL);
  return f;
}

static inline struct file *fd_lookup(files_t *files, int fd) {
  return rcu_dereference(files->fd_table[fd]);
}

#endif // KERNEL_BSD_TYPES_H

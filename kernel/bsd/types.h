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
#include "kernel/xcore/mm_types.h" // mm, mmap_region, shm
#include "kernel/xcore/rcu.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/wait_queue.h"
#include "kernel/xcore/xtask.h" // pid_t, xtask
#include <stdint.h>
#include <xos/fcntl.h>
#include <xos/mman.h>
#include <xos/types.h>

// ===================== fd / pipe =====================
#define MAX_FD 128
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
#define FD_EPOLL 9
#define FD_EVENTFD 10
#define FD_TIMERFD 11
#define FD_SIGNALFD 12
#define FD_NETLINK 13

typedef struct pipe {
  uint8_t *buf;
  uint32_t head;
  uint32_t tail;
  pid_t read_pid;
  pid_t write_pid;
  refcount_t p_count;
  wait_queue_head *close_wq; // 惰性分配，epoll 等待者挂此
} pipe;

struct unix_sock;
struct inode;
struct pty;
struct eventpoll;
struct eventfd_ctx;
struct timerfd_ctx;
struct signalfd_ctx;
struct netlink_sock;

typedef struct file {
  refcount_t f_count;
  int type;
  int flags;
  struct inode *inode;
  uint64_t offset;
  wait_queue_head *wq; // 惰性分配：NULL 表示无等待者
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
    struct eventpoll *epoll;
    struct eventfd_ctx *eventfd;
    struct timerfd_ctx *timerfd;
    struct signalfd_ctx *signalfd;
    struct netlink_sock *nlsock;
  };
} file;

typedef struct files {
  spinlock fd_lock;
  struct file *fd_table[MAX_FD];
  refcount_t f_count;
} files;

// mm, mmap_region, shm defined in kernel/xcore/mm_types.h
// shm_get/shm_put declared in kernel/xcore/trap.h
// copy_page_table/add_mmap_region declared in kernel/xcore/kpi.h

// ===================== files lifecycle =====================
files *files_create(void);
void files_put(files *files);

// ===================== unified fd lifecycle =====================
void file_put(struct file *f);
int alloc_fd(files *files, int min_fd);
void pty_dup_file(struct file *f);
void pty_close_file(struct file *f);

static inline void file_get(struct file *f) {
  if (f)
    refcount_inc(&f->f_count);
}

static inline void fd_install(files *files, int fd, struct file *f) {
  RCU_ASSIGN_POINTER(files->fd_table[fd], f);
}

static inline struct file *fd_uninstall(files *files, int fd) {
  struct file *f = files->fd_table[fd];
  RCU_ASSIGN_POINTER(files->fd_table[fd], NULL);
  return f;
}

static inline struct file *fd_lookup(files *files, int fd) {
  return RCU_DEREFERENCE(files->fd_table[fd]);
}

#endif // KERNEL_BSD_TYPES_H

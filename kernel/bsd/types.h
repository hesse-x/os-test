/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_BSD_TYPES_H
#define KERNEL_BSD_TYPES_H

#include "kernel/bsd/kfcntl.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/mm_types.h" // mm, mmap_region, shm
#include "kernel/xcore/rcu.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/wait_queue.h"
#include "kernel/xcore/xtask.h" // pid_t, xtask
#include <stdint.h>
#include <xos/mman.h>
#include <xos/types.h>

#include "kernel/bsd/fops.h"

// ===================== fd / pipe =====================
// MAX_FD aligns with Linux's RLIMIT_NOFILE soft default (1024). struct files is
// heap-allocated (files_create kmalloc), so the ~8KB fd_table/close_on_exec is
// per-process heap, not stack. The fd-collect-then-put snapshots in proc.c
// (proc_reap/files_put/execve cloexec) use kmalloc'd arrays rather than stack
// arrays to avoid blowing the 16KB kernel stack. Dynamic fdtable (rlimit +
// krealloc growth) is deferred to todo.md.
#define MAX_FD 1024
#define PIPE_BUF_SIZE 4096
// FD_CLOEXEC is the kernel-internal cloexec bit historically stored on the
// shared struct file (S06 moves it to a per-fd bitmap in struct files). It is
// kept only for the f->flags bit that pre-existing code paths may still
// reference during the transition; the source of truth is now the bitmap.
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
#define FD_IPC 14
#define FD_SYNC_FILE 15
#define FD_INOTIFY 16
#define FD_DRM_PRIME 17
#define FD_DMA_BUF 18

typedef struct pipe {
  uint8_t *buf;
  uint32_t size; // ring capacity (power-of-2, [PAGE_SIZE, PIPE_MAX_SIZE])
  uint32_t head;
  uint32_t tail;
  refcount_t p_count;
  wait_queue_head *wq; // eagerly allocated: shared by data-ready
                       // (POLLIN/POLLOUT) and close (POLLHUP)
} pipe;

struct unix_sock;
struct inode;
struct pty;
struct eventpoll;
struct eventfd_ctx;
struct timerfd_ctx;
struct signalfd_ctx;
struct netlink_sock;
struct drm_fence;
struct drm_prime_object;
struct dma_buf;
struct mount_entry;

typedef struct file {
  refcount_t f_count;
  atomic_t fd_refs;      // number of fd-table entries referring to this file
  list_node epoll_items; // epoll registrations targeting this file
  int epoll_items_initialized;
  int type;
  int flags;
  struct inode *inode;
  uint64_t offset;
  wait_queue_head *wq; // lazily allocated: NULL means no waiters
  const struct file_operations
      *f_op; // fd-I/O dispatch (NULL = type-based dispatch)
  struct mount_entry *mount;
  void *private_data; // Linux-style: used by broker/eventfd etc.; freed by
                      // f_op->close
  pid_t f_owner;   // F_SETOWN target pid (0 = none); stored, no SIGIO delivery
  int f_owner_sig; // F_SETSIG signal (0 → SIGIO default); stored, not delivered
  int f_owner_type; // F_OWNER_TID/PID/PGRP (F_SETOWN_EX); F_OWNER_PID for
                    // legacy F_SETOWN. Stored only (no SIGIO delivery).
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
    pid_t ipcfd_owner_pid; // FD_IPC: owner task pid whose recv queue this fd
                           // drains
    struct drm_fence
        *sync_file_fence; // FD_SYNC_FILE: bound fence (holds a ref)
    struct drm_prime_object *drm_prime;
    struct dma_buf *dma_buf;
  };
} file;

_Static_assert(offsetof(file, private_data) == 80,
               "struct file ABI changed - update driver/bsd_types.h");

// S06: per-fd close-on-exec bitmap. cloexec is an fd-level attribute, but the
// old design stored it on the refcounted/shared struct file (so dup'd fds
// shared one bit — F_DUPFD_CLOEXEC/dup3 wrongly flipped the original fd too).
// The bitmap lives in struct files and is the single source of truth; struct
// file.flags no longer carries cloexec. Caller must hold files->fd_lock.
typedef struct files {
  spinlock fd_lock;
  struct file *fd_table[MAX_FD];
  uint64_t close_on_exec[(MAX_FD + 63) / 64]; // bit fd => close on execve
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
void pty_close_file(struct file *f);

static inline void file_get(struct file *f) {
  if (f)
    refcount_inc(&f->f_count);
}

static inline void fd_install(files *files, int fd, struct file *f) {
  atomic_inc(&f->fd_refs);
  RCU_ASSIGN_POINTER(files->fd_table[fd], f);
}

static inline struct file *fd_uninstall(files *files, int fd) {
  struct file *f = files->fd_table[fd];
  RCU_ASSIGN_POINTER(files->fd_table[fd], NULL);
  if (f)
    atomic_dec(&f->fd_refs);
  return f;
}

static inline struct file *fd_lookup(files *files, int fd) {
  return RCU_DEREFERENCE(files->fd_table[fd]);
}

// S06: per-fd close-on-exec accessors. Call under files->fd_lock (the bitmap
// shares the fd-table lock; no separate lock to keep alloc/install/lookup and
// cloexec toggles mutually consistent).
static inline void fd_set_cloexec(files *fl, int fd, int on) {
  if (fd < 0 || fd >= MAX_FD)
    return;
  if (on)
    fl->close_on_exec[fd / 64] |= (1ULL << (fd % 64));
  else
    fl->close_on_exec[fd / 64] &= ~(1ULL << (fd % 64));
}

static inline int fd_get_cloexec(files *fl, int fd) {
  if (fd < 0 || fd >= MAX_FD)
    return 0;
  return (int)((fl->close_on_exec[fd / 64] >> (fd % 64)) & 1ULL);
}

#endif // KERNEL_BSD_TYPES_H

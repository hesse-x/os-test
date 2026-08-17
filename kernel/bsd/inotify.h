/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_BSD_INOTIFY_H
#define KERNEL_BSD_INOTIFY_H

#include <stddef.h>
#include <stdint.h>

#include "kernel/xcore/list.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/wait_queue.h"

// ===================== inotify (inode notify) =====================
// Kernel-side inotify subsystem: three syscalls (init1/add_watch/rm_watch) +
// a per-fd instance backed by struct file (type=FD_INOTIFY,
// private_data=inotify*). Watches are hung off a global inode→watch rbtree
// index (no inode struct invasion); VFS write paths call inotify_inode_event()
// to fan events out. See inotify.md for the full design contract.

// Event masks — mirror musl <sys/inotify.h> (kernel can't include the userspace
// header). Kept byte-for-byte in sync; trigger points pass these.
#define IN_ACCESS 0x00000001u
#define IN_MODIFY 0x00000002u
#define IN_ATTRIB 0x00000004u
#define IN_CLOSE_WRITE 0x00000008u
#define IN_CLOSE_NOWRITE 0x00000010u
#define IN_CLOSE (IN_CLOSE_WRITE | IN_CLOSE_NOWRITE)
#define IN_OPEN 0x00000020u
#define IN_MOVED_FROM 0x00000040u
#define IN_MOVED_TO 0x00000080u
#define IN_MOVE (IN_MOVED_FROM | IN_MOVED_TO)
#define IN_CREATE 0x00000100u
#define IN_DELETE 0x00000200u
#define IN_DELETE_SELF 0x00000400u
#define IN_MOVE_SELF 0x00000800u
#define IN_ALL_EVENTS 0x00000fffu
#define IN_UNMOUNT 0x00002000u
#define IN_Q_OVERFLOW 0x00004000u
#define IN_IGNORED 0x00008000u
#define IN_ONLYDIR 0x01000000u
#define IN_DONT_FOLLOW 0x02000000u
#define IN_EXCL_UNLINK 0x04000000u
#define IN_MASK_CREATE 0x10000000u
#define IN_MASK_ADD 0x20000000u
#define IN_ISDIR 0x40000000u
#define IN_ONESHOT 0x80000000u

// Flags accepted by inotify_init1.
#define IN_CLOEXEC 02000000 // == O_CLOEXEC (kfcntl.h)
#define IN_NONBLOCK 0x800   // == O_NONBLOCK

// Hard limits (style-aligned with EP_MAX_ITEMS=128 in eventpoll.h).
#define INOTIFY_MAX_WATCHES 128 // per-instance watch cap
#define INOTIFY_MAX_EVENTS 256  // per-instance event queue cap
#define INOTIFY_MAX_READ 4096   // single read byte cap
#define INOTIFY_NAME_MAX 255    // NAME_MAX truncation cap

// One watch: one inotify fd watching one inode with one mask.
typedef struct inotify_watch {
  list_node inode_node; // on the inode's watch list (via global index)
  list_node inst_node;  // on the owning instance's watch list
  struct inotify *inst; // owning instance (back-ref, for trigger enqueue)
  struct inode *inode; // watched inode (held via inode_get, paired on rm/close)
  uint32_t mask;       // user-registered event mask (IN_* subset)
  int wd;              // watch descriptor (instance-unique, >=1)
} inotify_watch;

// inotify instance (one inotify fd). wq is embedded (NOT f->wq lazy) so the
// trigger side reaches it directly via &inst->wq without an inst→file back-ref,
// avoiding a UAF across close — see inotify.md §2.4/§6.3.
typedef struct inotify {
  spinlock lock;         // guards watches + event_queue + nqueued + dropped
  list_node watches;     // this instance's watches (linear scan by wd)
  int next_wd;           // wd allocator (monotonic, wraps past INT_MAX)
  list_node event_queue; // ready events (inotify_event_node)
  int nqueued;           // current queue depth (vs INOTIFY_MAX_EVENTS)
  uint32_t dropped;      // overflow-drop count
  wait_queue_head wq;    // embedded, pre-initialized at create
} inotify;

// One queued event. read() unfolds this into struct inotify_event + name.
typedef struct inotify_event_node {
  list_node node;
  int wd;
  uint32_t mask;
  uint32_t cookie;
  uint32_t len; // padded name bytes including NUL; 0 for self-events
  char name[];  // flexible array, len bytes
} inotify_event_node;

struct file;
struct xtask;

// ===================== syscall entry points (dispatch cases)
// =====================
int64_t sys_inotify_init1(int64_t flags);
int64_t sys_inotify_add_watch(int64_t fd, int64_t pathname, int64_t mask);
int64_t sys_inotify_rm_watch(int64_t fd, int64_t wd);

// ===================== fd-type callbacks (type-chain dispatch)
// =====================
int64_t inotify_do_read(struct xtask *proc, struct file *f, void *buf,
                        size_t count);
uint32_t inotify_poll(struct file *f, uint32_t events);
void inotify_release(struct file *f);

// ===================== VFS trigger entry (called from fstype write paths)
// ===================== Fan out an inode event to all watches on `inode` whose
// mask intersects `mask`. `name` is the child entry name for directory events
// (may be NULL → len=0). NEVER fails: enqueue failure only bumps inst->dropped;
// the VFS main path is unaffected (callers do not check a return value). Lock
// order acquired internally: g_inotify_index_lock → inst->lock; MUST NOT be
// called with any fstype lock (fat_lock/i_lock) held — trigger points call
// after lock release.
void inotify_inode_event(struct inode *inode, uint32_t mask, uint32_t cookie,
                         const char *name);

#endif // KERNEL_BSD_INOTIFY_H

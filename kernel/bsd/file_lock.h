/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_BSD_FILE_LOCK_H
#define KERNEL_BSD_FILE_LOCK_H

// POSIX (fcntl) record locks — S09. per-inode lock list + conflict resolution.
// OFD locks (F_OFD_*) are placeholder -EINVAL (see uapi fcntl.h).

#include <stdint.h>

#include "kernel/xcore/xtask.h" // pid_t

struct file;
struct flock;
struct inode;

// Apply a fcntl(F_GETLK/SETLK/SETLKW) request to a regular file's inode.
// Resolves l_whence against f->offset / inode->size, probes/inserts/removes
// locks on inode->i_flock. F_SETLKW blocks on inode->wq (signal-interruptible).
// Returns 0 on success, or a negative -errno. On F_GETLK success the passed
// flock is updated (F_UNLCK if no conflict, else the conflicting lock) and the
// caller copies it back to userspace.
int64_t do_fcntl_lock(xtask *proc, struct file *f, int cmd, struct flock *lk);

// Release every POSIX lock owned by dead_pid across all inodes. Called from
// proc_reap so a dying process's locks are removed even if it never ran an
// explicit F_UNLCK (matching Linux's exit(2) lock cleanup).
void file_lock_release_pid(pid_t dead_pid);

// Release all locks on a single inode (inode eviction path).
void file_lock_release_all(struct inode *ip);

#endif // KERNEL_BSD_FILE_LOCK_H

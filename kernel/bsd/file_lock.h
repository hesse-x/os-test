/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_BSD_FILE_LOCK_H
#define KERNEL_BSD_FILE_LOCK_H

// POSIX (fcntl) record locks — S09. per-inode lock list + conflict resolution.
// OFD (F_OFD_*) locks (per open file description) share the same list and are
// released when the description's last fd closes (file_lock_release_file).

#include <stdint.h>

#include "kernel/xcore/xtask.h" // pid_t

struct file;
struct flock;
struct inode;

// Apply a fcntl(F_GETLK/SETLK/SETLKW) request to a regular file's inode.
// Resolves l_whence against f->offset / inode->size, probes/inserts/removes
// POSIX (per-process) locks on inode->i_flock. F_SETLKW blocks on inode->wq
// (signal-interruptible). Returns 0 on success, or a negative -errno. On
// F_GETLK success the passed flock is updated (F_UNLCK if no conflict, else the
// conflicting lock) and the caller copies it back to userspace.
int64_t do_fcntl_lock(xtask *proc, struct file *f, int cmd, struct flock *lk);

// Apply a fcntl(F_OFD_GETLK/SETLK/SETLKW) request. Same semantics as above but
// locks are owned by the open file description (struct file f) instead of the
// process: two independent open()s of the same file conflict, dup'd fds
// sharing one description do not. l_pid in F_OFD_GETLK reports the conflicting
// lock's creator pid (the lock itself tracks the file, not the pid).
int64_t do_fcntl_lock_ofd(xtask *proc, struct file *f, int cmd,
                          struct flock *lk);

// Release every POSIX lock owned by dead_pid across all inodes. Called from
// proc_reap so a dying process's locks are removed even if it never ran an
// explicit F_UNLCK (matching Linux's exit(2) lock cleanup). OFD locks are NOT
// released here — they outlive the process and are owned by the file.
void file_lock_release_pid(pid_t dead_pid);

// Release all locks on a single inode (inode eviction path).
void file_lock_release_all(struct inode *ip);

// Release every OFD lock owned by this open file description. Called from
// file_put on the last reference, before inode_put. OFD locks are tied to the
// file description, not the process, so they vanish when the description closes
// (the last dup'd fd), not when the creating process exits.
void file_lock_release_file(struct file *f);

#endif // KERNEL_BSD_FILE_LOCK_H

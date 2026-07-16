/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_BSD_IPCFD_H
#define KERNEL_BSD_IPCFD_H

#include <stddef.h>
#include <stdint.h>

#include "kernel/xcore/sparse.h"

struct file;

// Create an FD_IPC fd bound to the caller's own recv queue.  evdev-only by
// convention; other processes are unaffected.  Returns fd >= 0 or -errno.
int64_t sys_ipcfd_create(void);

// Non-blocking dequeue-from-owner-recv-queue read for an FD_IPC fd.
// Owner-checked: returns -EPERM if the caller is not the fd's owner.
// Returns 0 on a dequeued message, -EAGAIN if the owner queue is empty.
// Mirrors sys_recv's per-type handling via the shared ipc_dequeue (Stage 3).
int64_t ipcfd_do_read(struct file *f, void __user *buf, void __user *data_buf,
                      size_t data_buf_len);

// sys_ipcfd_read: dedicated 4-arg read syscall for FD_IPC fds.  The standard
// read(fd, buf, count) is only 3-arg, but ipcfd needs (msg, data_buf, len) —
// the recv_msg + variable-length RECV_IOCTL/RECV_MSG payload.  Owner-checked;
// returns 0 on dequeue, -EAGAIN when empty, -EPERM if not the owner.
int64_t sys_ipcfd_read(int64_t fd, int64_t buf, int64_t data_buf,
                       int64_t data_buf_len);

// Close hook: clear the owner task's ipcfd_file back-link and drop the
// create-time reference.  Called from file_put's FD_IPC teardown.
void ipcfd_close(struct file *f);

#endif // KERNEL_BSD_IPCFD_H

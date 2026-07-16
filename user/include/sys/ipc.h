/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _SYS_IPC_H
#define _SYS_IPC_H

#include <stdint.h>
#include <sys/cdefs.h>
#include <sys/poll.h>
#include <sys/types.h>
#include <syscall.h>

#ifdef __cplusplus
extern "C" {
#endif

LIBC_EXPORT int notify(pid_t pid);
LIBC_EXPORT int notify_fd(int fd);
LIBC_EXPORT int msg_fd(int fd, const void *msg_buf, size_t msg_len,
                       void *reply_buf, size_t reply_len);
LIBC_EXPORT int recv(struct recv_msg *msg, void *data_buf, size_t data_buf_len,
                     uint32_t timeout_ms);
LIBC_EXPORT int req(pid_t pid, void *req, void *resp);
LIBC_EXPORT int resp(void *resp, size_t len, int32_t result);
LIBC_EXPORT int msg(int32_t pid, void *req_buf, size_t req_len, void *resp_buf,
                    size_t resp_len);
LIBC_EXPORT int msg_resp(void *resp_buf, size_t resp_len);
LIBC_EXPORT int poll(struct pollfd *fds, nfds_t nfds, int timeout_ms);

// Create an FD_IPC fd bound to the caller's own recv queue (evdev's
// downstream-IPC fd).  ipcfd_read dequeues a recv_msg non-blockingly
// (-EAGAIN when empty); poll/epoll report POLLIN when the queue is non-empty.
// evdev-only by convention.  (evdev_refact.md §4.3)
LIBC_EXPORT int ipcfd_create(void);
// Dedicated 4-arg read for an ipcfd: read() is only 3-arg (fd, buf, count),
// but ipcfd needs the recv_msg target + variable-length payload (data_buf +
// len).  Returns 0 on dequeue, -1/-EAGAIN when empty (errno set).
LIBC_EXPORT int ipcfd_read(int fd, struct recv_msg *msg, void *data_buf,
                           size_t data_buf_len);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_IPC_H */

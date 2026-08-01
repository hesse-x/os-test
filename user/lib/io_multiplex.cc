/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * I/O multiplexing residuals: select, ipcfd (evdev downstream-IPC fd),
 * timerfd_create/timerfd_settime. epoll/eventfd/signalfd migrated to musl
 * src/linux (musl_linux_objs); select stays (src/select batch deferred).
 */

#include <errno.h> // IWYU pragma: keep
#include <stdint.h>
#include <xos/time.h>

#include <sys/cdefs.h>
#include <sys/poll.h>
#include <sys/select.h>
#include <sys/timerfd.h>
#include <xos/errno.h>
#include <xos/socket.h>
#include <xos/syscall_asm.h>
#include <xos/syscall_nums.h>

// ===================== select =====================
// Implemented on top of poll(2). musl's src/select/select.c is not yet compiled
// into libc (the src/select batch is deferred), so this poll-based select
// stays.
extern "C" int select(int nfds, fd_set *readfds, fd_set *writefds,
                      fd_set *exceptfds, struct timeval *timeout) {
  if (nfds < 0) {
    errno = EINVAL;
    return -1;
  }
  if (nfds > FD_SETSIZE)
    nfds = FD_SETSIZE;

  int timeout_ms;
  if (timeout) {
    timeout_ms = (int)(timeout->tv_sec * 1000 + timeout->tv_usec / 1000);
  } else {
    timeout_ms = -1;
  }

  struct pollfd fds[FD_SETSIZE];
  nfds_t count = 0;
  for (int i = 0; i < nfds; i++) {
    short events = 0;
    if (readfds && FD_ISSET(i, readfds))
      events |= POLLIN;
    if (writefds && FD_ISSET(i, writefds))
      events |= POLLOUT;
    if (exceptfds && FD_ISSET(i, exceptfds))
      events |= POLLERR;
    if (events) {
      fds[count].fd = i;
      fds[count].events = events;
      fds[count].revents = 0;
      count++;
    }
  }

  int ready = poll(fds, count, timeout_ms);
  if (ready < 0)
    return -1;

  if (readfds)
    FD_ZERO(readfds);
  if (writefds)
    FD_ZERO(writefds);
  if (exceptfds)
    FD_ZERO(exceptfds);

  int result = 0;
  for (nfds_t i = 0; i < count; i++) {
    int fd = fds[i].fd;
    short rev = fds[i].revents;

    if (readfds && (rev & (POLLIN | POLLHUP | POLLERR))) {
      FD_SET(fd, readfds);
      result++;
    }
    if (writefds && (rev & (POLLOUT | POLLHUP | POLLERR))) {
      FD_SET(fd, writefds);
      result++;
    }
    if (exceptfds && (rev & POLLERR)) {
      FD_SET(fd, exceptfds);
      result++;
    }
  }

  return result;
}

// ===================== ipcfd (evdev downstream-IPC fd) =====================

extern "C" int ipcfd_create(void) {
  int64_t ret = __syscall0(SYS_IPCFD_CREATE);
  if (ret < 0) {
    errno = (int)(-ret);
    return -1;
  }
  return (int)ret;
}

// Dedicated 4-arg read for an FD_IPC fd: read() is only 3-arg (fd, buf,
// count), but ipcfd needs the recv_msg target + variable-length payload
// (data_buf + len).  Dequeues non-blockingly; -EAGAIN (queue empty) → -1.
extern "C" int ipcfd_read(int fd, struct recv_msg *msg, void *data_buf,
                          size_t data_buf_len) {
  int64_t ret = __syscall4(SYS_IPCFD_READ, (int64_t)fd, (int64_t)(uintptr_t)msg,
                           (int64_t)(uintptr_t)data_buf, (int64_t)data_buf_len);
  if (ret < 0) {
    errno = (int)(-ret);
    return -1;
  }
  return (int)ret;
}

// ===================== timerfd =====================
//
// RETAINED: musl's src/linux/timerfd.c also defines timerfd_gettime, which
// routes to SYS_timerfd_gettime (unimplemented in this kernel → leaks an
// ENOSYS symbol into libc). Only timerfd_create/timerfd_settime are kept here
// (the kernel implements those two). Migrating the full file is deferred until
// SYS_timerfd_gettime lands; see libc_extend.md §2.2.

extern "C" LIBC_EXPORT int timerfd_create(int clockid, int flags) {
  int64_t ret =
      __syscall2(SYS_TIMERFD_CREATE, (int64_t)clockid, (int64_t)flags);
  if (ret < 0) {
    errno = (int)(-ret);
    return -1;
  }
  return (int)ret;
}

extern "C" LIBC_EXPORT int timerfd_settime(int fd, int flags,
                                           const struct itimerspec *new_value,
                                           struct itimerspec *old_value) {
  int64_t ret =
      __syscall4(SYS_TIMERFD_SETTIME, (int64_t)fd, (int64_t)flags,
                 (int64_t)(uintptr_t)new_value, (int64_t)(uintptr_t)old_value);
  if (ret < 0) {
    errno = (int)(-ret);
    return -1;
  }
  return (int)ret;
}

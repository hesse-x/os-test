/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * I/O multiplexing: select, epoll, eventfd, timerfd, signalfd
 *
 * Merged from select.c + epoll.cc + eventfd.cc + timerfd.cc + signalfd.cc
 */

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/poll.h>
#include <sys/select.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <xos/errno.h>
#include <xos/socket.h>
#include <xos/syscall_asm.h>
#include <xos/syscall_nums.h>

// ===================== select =====================
// Implemented on top of poll(2).

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

// ===================== epoll =====================

extern "C" int epoll_create(int size) {
  (void)size;
  int64_t ret = __syscall1(SYS_EPOLL_CREATE, (int64_t)size);
  if (ret < 0) {
    errno = (int)(-ret);
    return -1;
  }
  return (int)ret;
}

extern "C" int epoll_create1(int flags) {
  int64_t ret = __syscall1(SYS_EPOLL_CREATE1, (int64_t)flags);
  if (ret < 0) {
    errno = (int)(-ret);
    return -1;
  }
  return (int)ret;
}

extern "C" int epoll_ctl(int epfd, int op, int fd, struct epoll_event *ev) {
  int64_t ret = __syscall4(SYS_EPOLL_CTL, (int64_t)epfd, (int64_t)op,
                           (int64_t)fd, (int64_t)(uintptr_t)ev);
  if (ret < 0) {
    errno = (int)(-ret);
    return -1;
  }
  return (int)ret;
}

extern "C" int epoll_wait(int epfd, struct epoll_event *events, int maxevents,
                          int timeout) {
  int64_t ret =
      __syscall4(SYS_EPOLL_WAIT, (int64_t)epfd, (int64_t)(uintptr_t)events,
                 (int64_t)maxevents, (int64_t)timeout);
  if (ret < 0) {
    errno = (int)(-ret);
    return -1;
  }
  return (int)ret;
}

extern "C" int epoll_pwait(int epfd, struct epoll_event *events, int maxevents,
                           int timeout, const sigset_t *sigmask) {
  int64_t ret = __syscall6(SYS_EPOLL_PWAIT, (int64_t)epfd,
                           (int64_t)(uintptr_t)events, (int64_t)maxevents,
                           (int64_t)timeout, (int64_t)(uintptr_t)sigmask,
                           sigmask ? (int64_t)sizeof(sigset_t) : 0);
  if (ret < 0) {
    errno = (int)(-ret);
    return -1;
  }
  return (int)ret;
}

// ===================== eventfd =====================

extern "C" int eventfd(unsigned int initval, int flags) {
  int64_t ret = __syscall2(SYS_EVENTFD2, (int64_t)initval, (int64_t)flags);
  if (ret < 0) {
    errno = (int)(-ret);
    return -1;
  }
  return (int)ret;
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

extern "C" int timerfd_create(int clockid, int flags) {
  int64_t ret =
      __syscall2(SYS_TIMERFD_CREATE, (int64_t)clockid, (int64_t)flags);
  if (ret < 0) {
    errno = (int)(-ret);
    return -1;
  }
  return (int)ret;
}

extern "C" int timerfd_settime(int fd, int flags,
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

// ===================== signalfd =====================

extern "C" int signalfd(int fd, const sigset_t *mask, int flags) {
  int64_t ret = __syscall4(SYS_SIGNALFD4, (int64_t)fd, (int64_t)(uintptr_t)mask,
                           (int64_t)sizeof(sigset_t), (int64_t)flags);
  if (ret < 0) {
    errno = (int)(-ret);
    return -1;
  }
  return (int)ret;
}

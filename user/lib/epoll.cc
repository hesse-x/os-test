/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// libc epoll: thin syscall wrappers over SYS_EPOLL_* (kernel-implemented).
#include <signal.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <syscall.h>
#include <xos/syscall_asm.h>
#include <xos/syscall_nums.h>

int epoll_create(int size) {
  (void)size;
  int64_t ret = __syscall1(SYS_EPOLL_CREATE, (int64_t)size);
  if (ret < 0) {
    errno = (int)(-ret);
    return -1;
  }
  return (int)ret;
}

int epoll_create1(int flags) {
  int64_t ret = __syscall1(SYS_EPOLL_CREATE1, (int64_t)flags);
  if (ret < 0) {
    errno = (int)(-ret);
    return -1;
  }
  return (int)ret;
}

int epoll_ctl(int epfd, int op, int fd, struct epoll_event *ev) {
  int64_t ret = __syscall4(SYS_EPOLL_CTL, (int64_t)epfd, (int64_t)op,
                           (int64_t)fd, (int64_t)(uintptr_t)ev);
  if (ret < 0) {
    errno = (int)(-ret);
    return -1;
  }
  return (int)ret;
}

int epoll_wait(int epfd, struct epoll_event *events, int maxevents,
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

int epoll_pwait(int epfd, struct epoll_event *events, int maxevents,
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

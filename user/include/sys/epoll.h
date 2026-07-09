/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _SYS_EPOLL_H
#define _SYS_EPOLL_H

#include <signal.h>
#include <sys/cdefs.h>
#include <xos/epoll.h>

#ifdef __cplusplus
extern "C" {
#endif

LIBC_EXPORT int epoll_create(int size);
LIBC_EXPORT int epoll_create1(int flags);
LIBC_EXPORT int epoll_ctl(int epfd, int op, int fd, struct epoll_event *ev);
LIBC_EXPORT int epoll_wait(int epfd, struct epoll_event *events, int maxevents,
                           int timeout);
LIBC_EXPORT int epoll_pwait(int epfd, struct epoll_event *events, int maxevents,
                            int timeout, const sigset_t *sigmask);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_EPOLL_H */

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _SYS_POLL_H
#define _SYS_POLL_H

#include <sys/cdefs.h>
#include <sys/types.h>
#include <xos/socket.h> // defines struct pollfd, POLLIN/OUT/ERR/HUP

#ifdef __cplusplus
extern "C" {
#endif

LIBC_EXPORT int poll(struct pollfd *fds, nfds_t nfds, int timeout_ms);

// ppoll — poll with a timespec timeout and an atomic signal-mask swap.
// Declared unconditionally (like epoll_pwait in <sys/epoll.h>) so the
// LIBC_EXPORT prototype is visible to the libc.so definition even though
// file.cc does not define _GNU_SOURCE; libwayland-client dlopen's this.
#define __NEED_time_t
#define __NEED_struct_timespec
#define __NEED_sigset_t
#include <bits/alltypes.h>
LIBC_EXPORT int ppoll(struct pollfd *fds, nfds_t nfds,
                      const struct timespec *timeout_ts,
                      const sigset_t *sigmask);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_POLL_H */

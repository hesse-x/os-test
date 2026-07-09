/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _SYS_TIMERFD_H
#define _SYS_TIMERFD_H

#include <sys/cdefs.h>
#include <time.h>

#define TFD_CLOEXEC 0x8000
#define TFD_NONBLOCK 0x800
#define TFD_TIMER_ABSTIME 0x01
#define CLOCK_MONOTONIC 1

struct itimerspec {
  struct timespec it_interval;
  struct timespec it_value;
};

#ifdef __cplusplus
extern "C" {
#endif

LIBC_EXPORT int timerfd_create(int clockid, int flags);
LIBC_EXPORT int timerfd_settime(int fd, int flags,
                                const struct itimerspec *new_value,
                                struct itimerspec *old_value);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_TIMERFD_H */

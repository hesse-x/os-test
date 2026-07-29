/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * Thin shim forwarding to the unmodified musl time.h. musl declares the full
 * POSIX/GNU time surface (struct tm with the __tm_gmtoff and __tm_zone fields
 * that musl gmtime_r/mktime/strftime write, struct itimerspec, all CLOCK and
 * TIMER constants, TIMER_ABSTIME, the timer family, strptime, getdate,
 * timegm, tzset, timezone, daylight, tzname) and pulls time_t, clock_t,
 * timer_t, locale_t, and struct timespec via bits/alltypes.h (the checked-in
 * user/include/bits/alltypes.h). The old repo time.h defined a UTC-only
 * struct tm WITHOUT the __tm_gmtoff/__tm_zone fields, which the now-adopted
 * musl src/time sources (musl_time_objs) write to, so it had to go. Same
 * shim pattern as sys/time.h. The guard name is deliberately NOT _TIME_H:
 * musl header uses that, and reusing it would make its ifndef skip the body.
 *
 * The kernel-side shared UAPI stays in xos/time.h (struct timespec, struct
 * timeval, time_t, the CLOCK constants, for the kernel, which does not
 * include musl headers); its __DEFINED_ guards coexist with musl alltypes
 * so the two never double-define when both are pulled into one userspace TU
 * (e.g. via syscall.h).
 */
#ifndef _USER_TIME_SHIM_H
#define _USER_TIME_SHIM_H
#include "musl/include/time.h"

/* Compat: the old repo <time.h> pulled in <xos/time.h>, which defined
 * struct timeval alongside struct timespec. musl's <time.h> only defines
 * struct timespec (POSIX puts struct timeval in <sys/time.h>/<sys/select.h>).
 * Several in-tree sources historically rely on <time.h> providing timeval
 * (evdev.cc, test_clock_realtime.c, io_multiplex.cc via <sys/select.h>).
 * Preserve that contract by pulling struct timeval from bits/alltypes.h
 * here. Idempotent via __DEFINED_struct_timeval, so a later <sys/time.h>
 * re-include is a no-op. suseconds_t is pulled alongside (timeval's
 * tv_usec uses it). */
#define __NEED_suseconds_t
#define __NEED_struct_timeval
#include <bits/alltypes.h>
#endif

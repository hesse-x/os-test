/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef COMMON_TIME_H
#define COMMON_TIME_H

// ===================================================================
// struct timespec / struct timeval / time_t / suseconds_t are layout-
// identical between this OS and musl on x86-64 (time_t=long, suseconds_t=
// long, {long tv_sec; long tv_nsec}, {long tv_sec; long tv_usec}). So both
// the kernel and userspace can share one definition — we just must not
// define it twice in a single TU.
//
// musl's <bits/alltypes.h> defines these under the guard pattern
//   #if defined(__NEED_struct_timespec) && !defined(__DEFINED_struct_timespec)
//   ... define ...  #define __DEFINED_struct_timespec
//   #endif
// We mirror that: if musl already defined the type, skip; otherwise define
// it AND set the __DEFINED_* guard so a later musl alltypes.h skips its own
// (preventing "xos first, musl second" double-definition). The kernel (no
// musl headers, no __NEED_*) always takes our definition.
// ===================================================================

#ifndef __DEFINED_time_t
#define __DEFINED_time_t
typedef long time_t;
#endif

#ifndef __DEFINED_struct_timespec
#define __DEFINED_struct_timespec
struct timespec {
  time_t tv_sec;
  long tv_nsec;
};
#endif

#ifndef __DEFINED_struct_timeval
#define __DEFINED_struct_timeval
struct timeval {
  time_t tv_sec;
  long tv_usec;
};
#endif

// ===================== clock_gettime clock IDs (aligned with Linux)
// =====================
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID 3
#define CLOCK_MONOTONIC_RAW 4
#define CLOCK_REALTIME_COARSE 5
#define CLOCK_MONOTONIC_COARSE 6
#define CLOCK_BOOTTIME 7
#define CLOCK_TAI 11

// clock_nanosleep flags (aligned with Linux bit/timerfd.h)
#define TIMER_ABSTIME 1

#endif /* COMMON_TIME_H */

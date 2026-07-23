/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef COMMON_TIME_H
#define COMMON_TIME_H

typedef long time_t;

struct timespec {
  time_t tv_sec;
  long tv_nsec;
};

struct timeval {
  time_t tv_sec;
  long tv_usec;
};

// ===================== clock_gettime clock IDs (对齐 Linux)
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

// clock_nanosleep flags (对齐 Linux bit/timerfd.h)
#define TIMER_ABSTIME 1

#endif /* COMMON_TIME_H */

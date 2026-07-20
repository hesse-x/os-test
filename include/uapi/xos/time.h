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

#endif /* COMMON_TIME_H */

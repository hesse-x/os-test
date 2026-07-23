/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _TIME_H
#define _TIME_H

#include <stddef.h>
#include <stdint.h>
#include <sys/cdefs.h>
#include <xos/time.h>

typedef long clock_t;
typedef long time_t;
typedef int clockid_t;

#define CLOCKS_PER_SEC 1000000

#define TIME_UTC 1

/* Calendar time (UTC-only, D10: system has no timezone, localtime = gmtime) */
struct tm {
  int tm_sec;   /* seconds [0-60] (60 = leap second) */
  int tm_min;   /* minutes [0-59] */
  int tm_hour;  /* hours [0-23] */
  int tm_mday;  /* day of month [1-31] */
  int tm_mon;   /* month [0-11] */
  int tm_year;  /* year - 1900 */
  int tm_wday;  /* day of week [0-6], 0=Sunday */
  int tm_yday;  /* day of year [0-365] */
  int tm_isdst; /* daylight saving flag (always 0 in this OS) */
};

#ifdef __cplusplus
extern "C" {
#endif

/* Timezone stub (D10: no timezone, fixed UTC) */
LIBC_EXPORT extern long timezone;
LIBC_EXPORT extern int daylight;
LIBC_EXPORT extern char *tzname[2];

LIBC_EXPORT int timespec_get(struct timespec *ts, int base);
LIBC_EXPORT clock_t clock(void);
LIBC_EXPORT int nanosleep(const struct timespec *req, struct timespec *rem);
LIBC_EXPORT int clock_nanosleep(clockid_t clk, int flags,
                                const struct timespec *req,
                                struct timespec *rem);
LIBC_EXPORT int usleep(unsigned usec);

/* clock_gettime / gettimeofday (wraps sys_clock_gettime) */
LIBC_EXPORT int clock_gettime(int clk, struct timespec *ts);
LIBC_EXPORT int clock_settime(clockid_t clk, const struct timespec *ts);
LIBC_EXPORT int gettimeofday(struct timeval *tv, void *tz);

/* Calendar conversion (UTC-only) */
LIBC_EXPORT time_t time(time_t *t);
LIBC_EXPORT struct tm *gmtime(const time_t *t);
LIBC_EXPORT struct tm *gmtime_r(const time_t *t, struct tm *result);
LIBC_EXPORT struct tm *localtime(const time_t *t);
LIBC_EXPORT struct tm *localtime_r(const time_t *t, struct tm *result);
LIBC_EXPORT time_t mktime(struct tm *tm);
LIBC_EXPORT struct tm *timespec_to_tm(const struct timespec *ts,
                                      struct tm *result);

/* Formatting */
LIBC_EXPORT size_t strftime(char *buf, size_t max, const char *fmt,
                            const struct tm *tm);
LIBC_EXPORT char *asctime_r(const struct tm *tm, char *buf);
LIBC_EXPORT char *asctime(const struct tm *tm);
LIBC_EXPORT char *ctime_r(const time_t *t, char *buf);
LIBC_EXPORT char *ctime(const time_t *t);
LIBC_EXPORT double difftime(time_t a, time_t b);
LIBC_EXPORT void tzset(void);

#ifdef __cplusplus
}
#endif

#endif /* _TIME_H */

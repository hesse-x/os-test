#ifndef _TIME_H
#define _TIME_H

#include <stddef.h>
#include <stdint.h>
#include <sys/cdefs.h>
#include <xos/time.h>

typedef long clock_t;
typedef long time_t;

#define CLOCKS_PER_SEC 1000000

#define CLOCK_MONOTONIC 1
#define CLOCK_REALTIME 0
#define TIME_UTC 1

/* 日历时间（UTC-only，D10：系统无时区，localtime = gmtime） */
struct tm {
  int tm_sec;   /* 秒 [0-60]（60 = 闰秒） */
  int tm_min;   /* 分 [0-59] */
  int tm_hour;  /* 小时 [0-23] */
  int tm_mday;  /* 月内日 [1-31] */
  int tm_mon;   /* 月 [0-11] */
  int tm_year;  /* 年 - 1900 */
  int tm_wday;  /* 周内日 [0-6]，0=周日 */
  int tm_yday;  /* 年内日 [0-365] */
  int tm_isdst; /* 夏令时标志（本 OS 恒 0） */
};

#ifdef __cplusplus
extern "C" {
#endif

/* 时区桩（D10：无时区，固定 UTC） */
LIBC_EXPORT extern long timezone;
LIBC_EXPORT extern int daylight;
LIBC_EXPORT extern char *tzname[2];

LIBC_EXPORT int timespec_get(struct timespec *ts, int base);
LIBC_EXPORT clock_t clock(void);
LIBC_EXPORT int nanosleep(const struct timespec *req, struct timespec *rem);

/* clock_gettime / gettimeofday（包 sys_gettime） */
LIBC_EXPORT int clock_gettime(int clk, struct timespec *ts);
LIBC_EXPORT int gettimeofday(struct timeval *tv, void *tz);

/* 日历换算（UTC-only） */
LIBC_EXPORT time_t time(time_t *t);
LIBC_EXPORT struct tm *gmtime(const time_t *t);
LIBC_EXPORT struct tm *gmtime_r(const time_t *t, struct tm *result);
LIBC_EXPORT struct tm *localtime(const time_t *t);
LIBC_EXPORT struct tm *localtime_r(const time_t *t, struct tm *result);
LIBC_EXPORT time_t mktime(struct tm *tm);
LIBC_EXPORT struct tm *timespec_to_tm(const struct timespec *ts,
                                      struct tm *result);

/* 格式化 */
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

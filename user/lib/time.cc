/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>
#include <syscall.h>
#include <time.h>
#include <unistd.h>

#include <sys/ipc.h>
#include <xos/errno.h>
#include <xos/syscall_nums.h>
#include <xos/time.h>

extern "C" {

/* ===================== Basics: clock_gettime-backed time getters
 * =====================
 */

int timespec_get(struct timespec *ts, int base) {
  if (base != TIME_UTC)
    return 0;

  if (sys_clock_gettime(CLOCK_REALTIME, ts) != 0)
    return 0;
  return base;
}

clock_t clock(void) {
  struct timespec ts;
  if (sys_clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0)
    return (clock_t)-1;
  uint64_t cpu_time_ns =
      (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
  return (clock_t)(cpu_time_ns / 1000);
}

int clock_gettime(int clk, struct timespec *ts) {
  if (sys_clock_gettime(clk, ts) != 0)
    return -1;
  return 0;
}

int gettimeofday(struct timeval *tv, void *tz) {
  (void)tz;
  struct timespec ts;
  if (sys_clock_gettime(CLOCK_REALTIME, &ts) != 0)
    return -1;
  tv->tv_sec = ts.tv_sec;
  tv->tv_usec = ts.tv_nsec / 1000;
  return 0;
}

time_t time(time_t *t) {
  struct timespec ts;
  if (sys_clock_gettime(CLOCK_REALTIME, &ts) != 0)
    return (time_t)-1;
  time_t sec = ts.tv_sec;
  if (t)
    *t = sec;
  return sec;
}

/* ===================== Timezone stubs (D10: UTC-only) ===================== */

long timezone = 0;
int daylight = 0;
char *tzname[2] = {(char *)"UTC", (char *)"UTC"};

void tzset(void) { /* No timezone, no-op stub */ }

/* ===================== time_t → struct tm calendar conversion (UTC)
 * =====================
 *
 * Algorithm: days since 1970-01-01 + seconds within the day. Reverse the
 * 400-year/100-year/4-year leap rules to derive year/month/day. tm_wday/tm_yday
 * are computed along the way. About 80 lines, no external dependencies.
 */
static void secs_to_tm(time_t t, struct tm *tm) {
  /* Handle negative times (t < 0): long division guarantees floor division */
  long long days = (long long)(t / 86400);
  long long rem =
      (long long)t - days * 86400; /* remaining seconds of the day [0,86400) */

  tm->tm_sec = (int)(rem % 60);
  rem /= 60;
  tm->tm_min = (int)(rem % 60);
  tm->tm_hour = (int)(rem / 60);

  /* 1970-01-01 is a Thursday (wday=4) */
  long long wday = (days % 7 + 4 + 7) % 7;
  tm->tm_wday = (int)wday;

  /* Fold days into year/month/day starting from 1970 */
  long long year = 1970;
  long long ydays;
  for (;;) {
    long long leap = ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0));
    long long diy = leap ? 366 : 365;
    if (days >= diy) {
      days -= diy;
      year++;
    } else if (days < 0) {
      year--;
      leap = ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0));
      days += (leap ? 366 : 365);
    } else {
      ydays = days;
      break;
    }
  }
  tm->tm_yday = (int)ydays;
  tm->tm_year = (int)(year - 1900);

  static const int mdays[2][12] = {
      {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}, /* common year */
      {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}, /* leap year */
  };
  int leap = ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)) ? 1 : 0;
  int mon = 0;
  while (mon < 11 && ydays >= mdays[leap][mon]) {
    ydays -= mdays[leap][mon];
    mon++;
  }
  tm->tm_mon = mon;
  tm->tm_mday = (int)ydays + 1;
  tm->tm_isdst = 0;
}

struct tm *gmtime_r(const time_t *t, struct tm *result) {
  secs_to_tm(*t, result);
  return result;
}

struct tm *gmtime(const time_t *t) {
  static struct tm buf;
  return gmtime_r(t, &buf);
}

/* D10: no timezone, localtime = gmtime */
struct tm *localtime_r(const time_t *t, struct tm *result) {
  return gmtime_r(t, result);
}

struct tm *localtime(const time_t *t) { return gmtime(t); }

struct tm *timespec_to_tm(const struct timespec *ts, struct tm *result) {
  secs_to_tm((time_t)ts->tv_sec, result);
  return result;
}

/* mktime: struct tm → time_t.
 *
 * POSIX semantics: tm fields may be out of range (tm_mday=0 means the last day
 * of the previous month, tm_mon=-1 means December of the previous year, etc.).
 * Algorithm — fold each field into total seconds since 1970-01-01 00:00:00 UTC:
 * first sum whole-year days up to the target year (including leap years), then
 * add days month by month to the target month, then add day/hour/minute/second.
 * Finally use secs_to_tm to backfill the normalized fields.
 *
 * Note tm_year is year - 1900, tm_mon is [0-11] but may be out of range.
 */
static int is_leap(long long y) {
  return ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) ? 1 : 0;
}

static int month_days(long long y, int m) {
  static const int mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 1)
    return is_leap(y) ? 29 : 28;
  return mdays[m];
}

time_t mktime(struct tm *tm) {
  long long year = tm->tm_year + 1900LL;
  long long mon = tm->tm_mon;  /* may be out of range */
  long long day = tm->tm_mday; /* may be out of range */
  /* Normalize out-of-range month into [0,11], carrying into year */
  if (mon < 0 || mon > 11) {
    long long adj = mon / 12;
    mon -= adj * 12;
    year += adj;
    if (mon < 0) {
      mon += 12;
      year -= 1;
    }
  }
  tm->tm_mon = (int)mon;

  /* Sum whole-year days from 1970 up to year */
  long long days = 0;
  long long y = 1970;
  if (year >= 1970) {
    while (y < year) {
      days += is_leap(y) ? 366 : 365;
      y++;
    }
  } else {
    while (y > year) {
      y--;
      days -= is_leap(y) ? 366 : 365;
    }
  }
  /* Sum month days up to mon */
  for (int m = 0; m < mon; m++)
    days += month_days(year, m);
  /* Day (tm_mday is 1-based; out-of-range values are normalized by secs_to_tm
   * below) */
  days += day - 1;

  long long secs = days * 86400LL + (long long)tm->tm_hour * 3600LL +
                   (long long)tm->tm_min * 60LL + (long long)tm->tm_sec;

  /* Backfill normalized fields (wday/yday/mday/mon/year all recomputed) */
  secs_to_tm((time_t)secs, tm);
  return (time_t)secs;
}

double difftime(time_t a, time_t b) { return (double)a - (double)b; }

/* ===================== strftime =====================
 *
 * Supports common conversion specifiers: %Y %m %d %H %M %S %j %w %Z %z %A %B %p
 * %T(=HH:MM:SS) %F(=YYYY-MM-DD) %R(HH:MM) %D(MM/DD/YY) %s(epoch) %%.
 * Complex specifiers (%c %x %X localized formats) use the ISO C minimal
 * implementation returning placeholders like "%c".
 */
static int put_str(char **p, char *end, const char *s) {
  while (*s) {
    if (*p < end)
      **p = *s;
    (*p)++;
    s++;
  }
  return 0;
}

static int put_num(char **p, char *end, long long val, int width, int pad) {
  char tmp[32];
  int n = 0;
  if (val == 0)
    tmp[n++] = '0';
  while (val > 0 && n < 31) {
    tmp[n++] = '0' + (int)(val % 10);
    val /= 10;
  }
  while (n < width)
    tmp[n++] = pad;
  for (int i = n - 1; i >= 0; i--) {
    if (*p < end)
      **p = tmp[i];
    (*p)++;
  }
  return 0;
}

size_t strftime(char *buf, size_t max, const char *fmt, const struct tm *tm) {
  static const char *wday_name[7] = {"Sunday",    "Monday",   "Tuesday",
                                     "Wednesday", "Thursday", "Friday",
                                     "Saturday"};
  static const char *mon_name[12] = {
      "January", "February", "March",     "April",   "May",      "June",
      "July",    "August",   "September", "October", "November", "December"};
  char *p = buf;
  char *end = buf + max;
  (void)end;
  while (*fmt && p < end - 1) {
    if (*fmt != '%') {
      *p++ = *fmt++;
      continue;
    }
    fmt++;
    switch (*fmt) {
    case 'Y':
      put_num(&p, end, (long long)tm->tm_year + 1900, 4, '0');
      break;
    case 'm':
      put_num(&p, end, tm->tm_mon + 1, 2, '0');
      break;
    case 'd':
      put_num(&p, end, tm->tm_mday, 2, '0');
      break;
    case 'H':
      put_num(&p, end, tm->tm_hour, 2, '0');
      break;
    case 'M':
      put_num(&p, end, tm->tm_min, 2, '0');
      break;
    case 'S':
      put_num(&p, end, tm->tm_sec, 2, '0');
      break;
    case 'j':
      put_num(&p, end, tm->tm_yday + 1, 3, '0');
      break;
    case 'w':
      put_num(&p, end, tm->tm_wday, 1, '0');
      break;
    case 'Z':
      put_str(&p, end, "UTC");
      break;
    case 'z':
      put_str(&p, end, "+0000");
      break;
    case 'A':
      put_str(&p, end, wday_name[tm->tm_wday % 7]);
      break;
    case 'B':
      put_str(&p, end, mon_name[tm->tm_mon % 12]);
      break;
    case 'p':
      put_str(&p, end, tm->tm_hour < 12 ? "AM" : "PM");
      break;
    case 'T':
      put_num(&p, end, tm->tm_hour, 2, '0');
      *p++ = ':';
      put_num(&p, end, tm->tm_min, 2, '0');
      *p++ = ':';
      put_num(&p, end, tm->tm_sec, 2, '0');
      break;
    case 'F':
      put_num(&p, end, (long long)tm->tm_year + 1900, 4, '0');
      *p++ = '-';
      put_num(&p, end, tm->tm_mon + 1, 2, '0');
      *p++ = '-';
      put_num(&p, end, tm->tm_mday, 2, '0');
      break;
    case 'R':
      put_num(&p, end, tm->tm_hour, 2, '0');
      *p++ = ':';
      put_num(&p, end, tm->tm_min, 2, '0');
      break;
    case 'D':
      put_num(&p, end, tm->tm_mon + 1, 2, '0');
      *p++ = '/';
      put_num(&p, end, tm->tm_mday, 2, '0');
      *p++ = '/';
      put_num(&p, end, (tm->tm_year + 1900) % 100, 2, '0');
      break;
    case 's':
      put_num(&p, end, (long long)mktime((struct tm *)tm), 0, ' ');
      break;
    case '%':
      if (p < end)
        *p++ = '%';
      break;
    case '\0':
      goto done;
    default:
      if (p < end) {
        *p++ = '%';
        *p++ = *fmt;
      }
      break;
    }
    if (*fmt)
      fmt++;
  }
done:
  if (max > 0) {
    if (p >= end)
      p = end - 1;
    *p = '\0';
  }
  return (size_t)(p - buf);
}

/* ===================== asctime / ctime ===================== */

char *asctime_r(const struct tm *tm, char *buf) {
  static const char *wday[7] = {"Sun", "Mon", "Tue", "Wed",
                                "Thu", "Fri", "Sat"};
  static const char *mon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  /* "Www Mmm dd hh:mm:ss yyyy\n" (26 bytes including the trailing \n and NUL)
   */
  int h = tm->tm_hour, mi = tm->tm_min, s = tm->tm_sec;
  int d = tm->tm_mday, y = tm->tm_year + 1900;
  char tmp[26];
  int n = 0;
  const char *wd = wday[tm->tm_wday % 7];
  const char *mo = mon[tm->tm_mon % 12];
  tmp[n++] = wd[0];
  tmp[n++] = wd[1];
  tmp[n++] = wd[2];
  tmp[n++] = ' ';
  tmp[n++] = mo[0];
  tmp[n++] = mo[1];
  tmp[n++] = mo[2];
  tmp[n++] = ' ';
  /* dd padded with a space (asctime semantics: %e style) */
  if (d < 10) {
    tmp[n++] = ' ';
    tmp[n++] = '0' + d;
  } else {
    tmp[n++] = '0' + d / 10;
    tmp[n++] = '0' + d % 10;
  }
  tmp[n++] = ' ';
  tmp[n++] = '0' + h / 10;
  tmp[n++] = '0' + h % 10;
  tmp[n++] = ':';
  tmp[n++] = '0' + mi / 10;
  tmp[n++] = '0' + mi % 10;
  tmp[n++] = ':';
  tmp[n++] = '0' + s / 10;
  tmp[n++] = '0' + s % 10;
  tmp[n++] = ' ';
  tmp[n++] = '0' + (y / 1000) % 10;
  tmp[n++] = '0' + (y / 100) % 10;
  tmp[n++] = '0' + (y / 10) % 10;
  tmp[n++] = '0' + y % 10;
  tmp[n++] = '\n';
  tmp[n] = '\0';
  memcpy(buf, tmp, 26);
  return buf;
}

char *asctime(const struct tm *tm) {
  static char buf[26];
  return asctime_r(tm, buf);
}

char *ctime_r(const time_t *t, char *buf) {
  struct tm tmv;
  return asctime_r(gmtime_r(t, &tmv), buf);
}

char *ctime(const time_t *t) {
  static char buf[26];
  return ctime_r(t, buf);
}

/* ===================== sleep / usleep / nanosleep ===================== */

unsigned int sleep(unsigned seconds) {
  struct recv_msg msg;
  int r = recv(&msg, NULL, 0, seconds * 1000);
  if (r == -ETIMEDOUT)
    return 0;
  return 0;
}

int usleep(unsigned usec) {
  unsigned ms = usec / 1000;
  if (ms == 0)
    ms = 1;
  struct recv_msg msg;
  recv(&msg, NULL, 0, ms);
  return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
  if (!req)
    return -1;
  unsigned ms = (unsigned)(req->tv_sec * 1000 + req->tv_nsec / 1000000);
  if (ms == 0 && req->tv_nsec > 0)
    ms = 1;
  struct recv_msg msg;
  recv(&msg, NULL, 0, ms);
  if (rem) {
    rem->tv_sec = 0;
    rem->tv_nsec = 0;
  }
  return 0;
}

} /* extern "C" */

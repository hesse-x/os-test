/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <syscall.h>
#include <time.h>

extern "C" {

/* ===================== 基础：sys_gettime / sys_clock ===================== */

int timespec_get(struct timespec *ts, int base) {
  if (base != TIME_UTC)
    return 0;

  uint64_t ns = sys_gettime();
  ts->tv_sec = (time_t)(ns / 1000000000ULL);
  ts->tv_nsec = (long)(ns % 1000000000ULL);
  return base;
}

clock_t clock(void) {
  uint64_t cpu_time_ns = sys_clock();
  return (clock_t)(cpu_time_ns / 1000);
}

int clock_gettime(int clk, struct timespec *ts) {
  (void)clk;
  uint64_t ns = sys_gettime();
  ts->tv_sec = (time_t)(ns / 1000000000ULL);
  ts->tv_nsec = (long)(ns % 1000000000ULL);
  return 0;
}

int gettimeofday(struct timeval *tv, void *tz) {
  (void)tz;
  uint64_t ns = sys_gettime();
  tv->tv_sec = (time_t)(ns / 1000000000ULL);
  tv->tv_usec = (long)((ns / 1000ULL) % 1000000ULL);
  return 0;
}

time_t time(time_t *t) {
  uint64_t ns = sys_gettime();
  time_t s = (time_t)(ns / 1000000000ULL);
  if (t)
    *t = s;
  return s;
}

/* ===================== 时区桩（D10：UTC-only） ===================== */

long timezone = 0;
int daylight = 0;
char *tzname[2] = {(char *)"UTC", (char *)"UTC"};

void tzset(void) { /* 无时区，空操作桩 */
}

/* ===================== time_t → struct tm 日历换算（UTC）
 * =====================
 *
 * 算法：从 1970-01-01 起的天数 + 当天秒数。用 400 年/100 年/4 年的闰年规则
 * 反推年月日。tm_wday/tm_yday 顺带算出。约 80 行，无外部依赖。
 */
static void secs_to_tm(time_t t, struct tm *tm) {
  /* 处理负时间（t < 0）：用长除法保证地板除正确 */
  long long days = (long long)(t / 86400);
  long long rem = (long long)t - days * 86400; /* 当天剩余秒 [0,86400) */

  tm->tm_sec = (int)(rem % 60);
  rem /= 60;
  tm->tm_min = (int)(rem % 60);
  tm->tm_hour = (int)(rem / 60);

  /* 1970-01-01 是周四（wday=4） */
  long long wday = (days % 7 + 4 + 7) % 7;
  tm->tm_wday = (int)wday;

  /* 把 days 折算到 1970 年起点的年/月/日 */
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
      {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}, /* 平年 */
      {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}, /* 闰年 */
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

/* D10：无时区，localtime = gmtime */
struct tm *localtime_r(const time_t *t, struct tm *result) {
  return gmtime_r(t, result);
}

struct tm *localtime(const time_t *t) { return gmtime(t); }

struct tm *timespec_to_tm(const struct timespec *ts, struct tm *result) {
  secs_to_tm((time_t)ts->tv_sec, result);
  return result;
}

/* mktime：struct tm → time_t。
 *
 * POSIX 语义：tm 字段允许越界（tm_mday=0 表示上月末尾，tm_mon=-1 表示
 * 上年 12 月等）。算法——把各字段折算成自 1970-01-01 00:00:00 UTC 起的
 * 总秒数：先算年到目标年的整年天数（含闰年），再累加月到目标月的天数，
 * 再加日/时/分/秒。最后用 secs_to_tm 回填标准化字段。
 *
 * 注意 tm_year 是 year - 1900，tm_mon [0-11] 但允许越界。
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
  long long mon = tm->tm_mon;  /* 允许越界 */
  long long day = tm->tm_mday; /* 允许越界 */
  /* 把越界月归一到 [0,11]，进位到 year */
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

  /* 从 1970 累加整年天数到 year */
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
  /* 累加月天数到 mon */
  for (int m = 0; m < mon; m++)
    days += month_days(year, m);
  /* 日（tm_mday 是 1-based；越界由后续 secs_to_tm 标准化） */
  days += day - 1;

  long long secs = days * 86400LL + (long long)tm->tm_hour * 3600LL +
                   (long long)tm->tm_min * 60LL + (long long)tm->tm_sec;

  /* 回填标准化字段（wday/yday/mday/mon/year 全部重算） */
  secs_to_tm((time_t)secs, tm);
  return (time_t)secs;
}

double difftime(time_t a, time_t b) { return (double)a - (double)b; }

/* ===================== strftime =====================
 *
 * 支持常见转换说明符：%Y %m %d %H %M %S %j %w %Z %z %A %B %p
 * %T(=HH:MM:SS) %F(=YYYY-MM-DD) %R(HH:MM) %D(MM/DD/YY) %s(epoch) %% 。
 * 复杂说明符（%c %x %X 本地化格式）按 ISO C 最小实现返回 "%c" 等占位。
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
  /* "Www Mmm dd hh:mm:ss yyyy\n"（26 字节含末尾 \n 和 NUL） */
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
  /* dd 用空格填充（asctime 语义：%e 风格） */
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

} /* extern "C" */

#ifndef _TIME_H
#define _TIME_H

#include <stddef.h>

typedef long time_t;
typedef long clock_t;

#define CLOCKS_PER_SEC 1000000

#ifndef _STRUCT_TIMESPEC  // 宿主机 time.h 已定义则跳过
struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};
#define _STRUCT_TIMESPEC 1  // 防止宿主机 bits/types/struct_timespec.h 重复定义
#endif

#define CLOCK_MONOTONIC 1
#define TIME_UTC        1

#ifdef __cplusplus
extern "C" {
#endif

int timespec_get(struct timespec *ts, int base);
clock_t clock(void);
int nanosleep(const struct timespec *req, struct timespec *rem);

#ifdef __cplusplus
}
#endif

#endif

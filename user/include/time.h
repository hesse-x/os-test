#ifndef _TIME_H
#define _TIME_H

#include <stddef.h>

typedef long time_t;
typedef long clock_t;

#define CLOCKS_PER_SEC 1000000

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

#define CLOCK_MONOTONIC 1
#define TIME_UTC        1

#ifdef __cplusplus
extern "C" {
#endif

int timespec_get(struct timespec *ts, int base);
clock_t clock(void);

#ifdef __cplusplus
}
#endif

#endif

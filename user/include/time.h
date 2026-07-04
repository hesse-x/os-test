#ifndef _TIME_H
#define _TIME_H

#include <stddef.h>
#include "xos/time.h"

typedef long clock_t;

#define CLOCKS_PER_SEC 1000000

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

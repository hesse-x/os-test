#ifndef COMMON_TIME_H
#define COMMON_TIME_H

typedef long time_t;

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

#endif /* COMMON_TIME_H */

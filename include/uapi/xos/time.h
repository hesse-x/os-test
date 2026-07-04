#ifndef COMMON_TIME_H
#define COMMON_TIME_H

typedef long time_t;

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

struct timeval {
    time_t tv_sec;
    long   tv_usec;
};

#endif /* COMMON_TIME_H */

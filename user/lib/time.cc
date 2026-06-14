#include <time.h>
#include <common/syscall.h>

extern "C" {

int timespec_get(struct timespec *ts, int base) {
    if (base != TIME_UTC) return 0;

    uint64_t ns = sys_gettime();
    ts->tv_sec = (time_t)(ns / 1000000000ULL);
    ts->tv_nsec = (long)(ns % 1000000000ULL);
    return base;
}

clock_t clock(void) {
    uint64_t cpu_time_ns = sys_clock();
    return (clock_t)(cpu_time_ns / 1000);
}

}

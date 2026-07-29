/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// libc time — residual sleep/usleep wrappers.
//
// The rest of the <time.h> surface (clock_gettime/clock_settime/
// clock_nanosleep/nanosleep/gettimeofday/time/timespec_get/clock, gmtime/
// localtime/mktime/timegm, asctime/ctime, strftime/strptime, difftime,
// __tz timezone machinery) now comes from the upstream musl sources built as
// musl_time_objs (see user/CMakeLists.txt, time.md). Only sleep()/usleep()
// remain here: musl's sleep.c/usleep.c (src/unistd/) return the *remaining*
// interval on EINTR, but this OS wants the BSD sleep(3) "sleep the full
// duration" semantics — loop resuming the remainder after a signal. They
// layer on the now-musl nanosleep(). (This deliberate retention was set in
// the unistd migration: MUSL_UNISTD_EXCLUDE drops musl sleep.c/usleep.c.)

#include <errno.h>
#include <time.h>
#include <unistd.h>

#include <sys/cdefs.h>
#include <xos/errno.h>

extern "C" {

LIBC_EXPORT unsigned int sleep(unsigned int seconds) {
  struct timespec req = {(time_t)seconds, 0};
  struct timespec rem;
  while (nanosleep(&req, &rem) == -1 && errno == EINTR)
    req = rem; // resume the remaining interval after a signal
  return 0;
}

LIBC_EXPORT int usleep(unsigned usec) {
  struct timespec req;
  req.tv_sec = (time_t)(usec / 1000000U);
  req.tv_nsec = (long)((usec % 1000000U) * 1000U);
  struct timespec rem;
  while (nanosleep(&req, &rem) == -1 && errno == EINTR)
    req = rem;
  return 0;
}

} /* extern "C" */

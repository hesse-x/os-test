/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * Thin shim forwarding to the unmodified musl sys/timerfd.h. musl declares
 * timerfd_create/settime/gettime and pulls struct itimerspec (via <time.h>)
 * and the TFD_* constants (via <bits/timerfd.h>). The old repo header
 * re-defined struct itimerspec, which now multi-defines against musl's
 * <time.h> (musl_time_objs write itimerspec) — so it had to go. Same shim
 * pattern as sys/time.h. The guard name is deliberately NOT _SYS_TIMERFD_H:
 * musl header uses that, and reusing it would make its ifndef skip the body.
 * libc.map exports timerfd_create/timerfd_settime (timerfd_gettime is
 * declared but not provided — no kernel SYS_timerfd_gettime, 0 callers).
 */
#ifndef _USER_SYS_TIMERFD_SHIM_H
#define _USER_SYS_TIMERFD_SHIM_H
#include "musl/include/sys/timerfd.h"
#endif

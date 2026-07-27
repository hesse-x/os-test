/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * Thin shim forwarding to the unmodified musl <sys/time.h>. musl declares
 * gettimeofday/struct itimerval/utimes/... that third-party sources (expat,
 * wayland, mesa, libinput) expect; the old repo stub only pulled <xos/time.h>
 * and shadowed musl's version, so those sources saw gettimeofday undeclared.
 * Resolved via -I third_party (same pattern as <unistd.h>); struct timeval
 * arrives via musl's <sys/select.h> → <bits/alltypes.h>. The guard name is
 * deliberately NOT _SYS_TIME_H — musl's header uses that, and reusing it would
 * make its #ifndef skip the entire body.
 */
#ifndef _USER_SYS_TIME_SHIM_H
#define _USER_SYS_TIME_SHIM_H
#include "musl/include/sys/time.h"
#endif

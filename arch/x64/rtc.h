/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ARCH_X64_RTC_H
#define ARCH_X64_RTC_H

#include <stdint.h>

// CMOS MC146818 RTC time (broken-down calendar fields, UTC).
struct rtc_time {
  int year; // full 4-digit year (e.g. 2026)
  int mon;  // 1..12
  int mday; // 1..31
  int hour; // 0..23
  int min;  // 0..59
  int sec;  // 0..59
};

// Read the CMOS RTC and return Unix epoch seconds (UTC).  On failure (UIP
// stuck, unreadable CMOS) returns 0 and emits a WARN; callers must treat 0 as
// "no wall clock" and fall back to the monotonic clock.
uint64_t rtc_read_epoch_seconds(void);

// Wall-clock baseline anchored once at boot (apic_init, right after
// tsc_base = rdtsc64()).  CLOCK_REALTIME = wall_clock_boot_ns + sched_clock().
// Written once at boot under cli; thereafter read-only except clock_settime,
// which updates it atomically.  Read with __atomic_load RELAXED.
extern uint64_t wall_clock_boot_ns;

#endif // ARCH_X64_RTC_H

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_clock_realtime — S14: CLOCK_REALTIME wall clock (CMOS RTC) + clockid
 * set completion + clock_settime offset adjust. */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <unity.h>
#include <xos/errno.h>

void setUp(void) {}
void tearDown(void) {}

/* CR-001: CLOCK_REALTIME is a wall clock (epoch seconds), not the monotonic
 * "seconds since boot" value. A boot-time monotonic clock is usually < 1000s;
 * any post-2023 wall clock is > 1.7e9. */
void test_clock_realtime_is_epoch(void) {
  struct timespec ts;
  TEST_ASSERT_EQUAL_INT(0, clock_gettime(CLOCK_REALTIME, &ts));
  TEST_ASSERT_TRUE(ts.tv_sec > 1700000000L);
}

/* CR-002: clock_gettime(REALTIME) and gettimeofday agree to within 1s. */
void test_clock_realtime_matches_gettimeofday(void) {
  struct timespec ts;
  struct timeval tv;
  TEST_ASSERT_EQUAL_INT(0, clock_gettime(CLOCK_REALTIME, &ts));
  TEST_ASSERT_EQUAL_INT(0, gettimeofday(&tv, NULL));
  TEST_ASSERT_TRUE(llabs((long long)ts.tv_sec - (long long)tv.tv_sec) <= 1);
}

/* CR-003: monotonic < realtime (monotonic starts at 0 at boot). */
void test_monotonic_less_than_realtime(void) {
  struct timespec mono, rt;
  TEST_ASSERT_EQUAL_INT(0, clock_gettime(CLOCK_MONOTONIC, &mono));
  TEST_ASSERT_EQUAL_INT(0, clock_gettime(CLOCK_REALTIME, &rt));
  TEST_ASSERT_TRUE(mono.tv_sec < rt.tv_sec);
}

/* CR-004: MONOTONIC_RAW == MONOTONIC (no NTP in this OS). */
void test_monotonic_raw_equals_monotonic(void) {
  struct timespec a, b;
  TEST_ASSERT_EQUAL_INT(0, clock_gettime(CLOCK_MONOTONIC, &a));
  TEST_ASSERT_EQUAL_INT(0, clock_gettime(CLOCK_MONOTONIC_RAW, &b));
  TEST_ASSERT_TRUE(llabs((long long)a.tv_sec - (long long)b.tv_sec) <= 1);
}

/* CR-005: BOOTTIME == MONOTONIC (no suspend). */
void test_boottime_equals_monotonic(void) {
  struct timespec a, b;
  TEST_ASSERT_EQUAL_INT(0, clock_gettime(CLOCK_MONOTONIC, &a));
  TEST_ASSERT_EQUAL_INT(0, clock_gettime(CLOCK_BOOTTIME, &b));
  TEST_ASSERT_TRUE(llabs((long long)a.tv_sec - (long long)b.tv_sec) <= 1);
}

/* CR-006: TAI == REALTIME (no TAI offset). */
void test_tai_equals_realtime(void) {
  struct timespec a, b;
  TEST_ASSERT_EQUAL_INT(0, clock_gettime(CLOCK_REALTIME, &a));
  TEST_ASSERT_EQUAL_INT(0, clock_gettime(CLOCK_TAI, &b));
  TEST_ASSERT_TRUE(llabs((long long)a.tv_sec - (long long)b.tv_sec) <= 1);
}

/* CR-007: REALTIME_COARSE == REALTIME. */
void test_realtime_coarse_equals_realtime(void) {
  struct timespec a, b;
  TEST_ASSERT_EQUAL_INT(0, clock_gettime(CLOCK_REALTIME, &a));
  TEST_ASSERT_EQUAL_INT(0, clock_gettime(CLOCK_REALTIME_COARSE, &b));
  TEST_ASSERT_TRUE(llabs((long long)a.tv_sec - (long long)b.tv_sec) <= 1);
}

/* CR-008: MONOTONIC_COARSE == MONOTONIC. */
void test_monotonic_coarse_equals_monotonic(void) {
  struct timespec a, b;
  TEST_ASSERT_EQUAL_INT(0, clock_gettime(CLOCK_MONOTONIC, &a));
  TEST_ASSERT_EQUAL_INT(0, clock_gettime(CLOCK_MONOTONIC_COARSE, &b));
  TEST_ASSERT_TRUE(llabs((long long)a.tv_sec - (long long)b.tv_sec) <= 1);
}

/* CR-009: THREAD_CPUTIME_ID returns a valid (non-EINVAL) timespec. */
void test_thread_cputime_id_valid(void) {
  struct timespec ts;
  TEST_ASSERT_EQUAL_INT(0, clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts));
  TEST_ASSERT_TRUE(ts.tv_sec >= 0);
  TEST_ASSERT_TRUE(ts.tv_nsec >= 0 && ts.tv_nsec < 1000000000L);
}

/* CR-010: unknown clock id -> EINVAL. */
void test_unknown_clockid_einval(void) {
  struct timespec ts;
  TEST_ASSERT_EQUAL_INT(-1, clock_gettime(999, &ts));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
}

/* CR-011: clock_settime(REALTIME) shifts the offset; subsequent reads reflect
 * it. Restores the original time afterward so later tests aren't skewed. */
void test_clock_settime_adjusts_offset(void) {
  struct timespec before, after, shifted;
  TEST_ASSERT_EQUAL_INT(0, clock_gettime(CLOCK_REALTIME, &before));

  shifted.tv_sec = before.tv_sec + 1234;
  shifted.tv_nsec = before.tv_nsec;
  TEST_ASSERT_EQUAL_INT(0, clock_settime(CLOCK_REALTIME, &shifted));

  TEST_ASSERT_EQUAL_INT(0, clock_gettime(CLOCK_REALTIME, &after));
  /* Allow scheduling latency up to 2s. */
  TEST_ASSERT_TRUE(after.tv_sec >= shifted.tv_sec &&
                   after.tv_sec <= shifted.tv_sec + 2);

  /* Restore. */
  TEST_ASSERT_EQUAL_INT(0, clock_settime(CLOCK_REALTIME, &before));
}

/* CR-012: clock_settime(MONOTONIC) -> EPERM (monotonic is not settable). */
void test_clock_settime_monotonic_eperm(void) {
  struct timespec ts = {100, 0};
  TEST_ASSERT_EQUAL_INT(-1, clock_settime(CLOCK_MONOTONIC, &ts));
  TEST_ASSERT_EQUAL_INT(EPERM, errno);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_clock_realtime_is_epoch);
  RUN_TEST(test_clock_realtime_matches_gettimeofday);
  RUN_TEST(test_monotonic_less_than_realtime);
  RUN_TEST(test_monotonic_raw_equals_monotonic);
  RUN_TEST(test_boottime_equals_monotonic);
  RUN_TEST(test_tai_equals_realtime);
  RUN_TEST(test_realtime_coarse_equals_realtime);
  RUN_TEST(test_monotonic_coarse_equals_monotonic);
  RUN_TEST(test_thread_cputime_id_valid);
  RUN_TEST(test_unknown_clockid_einval);
  RUN_TEST(test_clock_settime_adjusts_offset);
  RUN_TEST(test_clock_settime_monotonic_eperm);
  return UNITY_END();
}

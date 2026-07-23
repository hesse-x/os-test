/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_clock_cputime — S15: CLOCK_PROCESS_CPUTIME_ID process-level CPU time
 * (sums the whole thread group, not just the calling thread). */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static uint64_t cputime_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Burn CPU for a short window so process CPU time advances measurably. */
static void burn(uint64_t ms_target) {
  uint64_t start = cputime_ns();
  volatile uint64_t acc = 0;
  while (cputime_ns() - start < ms_target * 1000000ULL)
    acc += 1;
  (void)acc;
}

/* CT-001: PROCESS_CPUTIME_ID advances while the process burns CPU. */
void test_process_cputime_advances(void) {
  uint64_t t0 = cputime_ns();
  burn(50); /* ~50ms of CPU */
  uint64_t t1 = cputime_ns();
  /* Single-thread: process cputime == this thread's cputime; allow slack. */
  TEST_ASSERT_TRUE(t1 - t0 >= 20 * 1000000ULL);
}

/* CT-002: PROCESS_CPUTIME_ID >= THREAD_CPUTIME_ID in a single-thread process
 * (they are equal when only one thread exists). */
void test_process_ge_thread_cputime(void) {
  struct timespec tp, th;
  TEST_ASSERT_EQUAL_INT(0, clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &tp));
  TEST_ASSERT_EQUAL_INT(0, clock_gettime(CLOCK_THREAD_CPUTIME_ID, &th));
  uint64_t p = (uint64_t)tp.tv_sec * 1000000000ULL + (uint64_t)tp.tv_nsec;
  uint64_t t = (uint64_t)th.tv_sec * 1000000000ULL + (uint64_t)th.tv_nsec;
  TEST_ASSERT_TRUE(p + 1000000000ULL >= t); /* p >= t within 1s slack */
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_process_cputime_advances);
  RUN_TEST(test_process_ge_thread_cputime);
  return UNITY_END();
}

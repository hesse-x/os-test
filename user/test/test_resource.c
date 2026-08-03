/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE
#include "unity.h"
#include <errno.h>
#include <sys/resource.h>

void setUp(void) {}
void tearDown(void) {}

void test_rlimit_round_trips(void) {
  // prlimit64 is implemented (kernel/bsd/syscall.c sys_prlimit64):
  // RLIMIT_NOFILE reports the kernel cap MAX_FD (1024) for both soft and hard.
  // getrlimit/ setrlimit/prlimit all route through SYS_prlimit64.
  struct rlimit limit = {0};
  errno = 0;
  TEST_ASSERT_EQUAL_INT(0, getrlimit(RLIMIT_NOFILE, &limit));
  TEST_ASSERT_EQUAL_INT(0, errno);
  TEST_ASSERT_EQUAL_INT(1024, limit.rlim_cur);
  TEST_ASSERT_EQUAL_INT(1024, limit.rlim_max);

  errno = 0;
  TEST_ASSERT_EQUAL_INT(0, prlimit(0, RLIMIT_NOFILE, NULL, &limit));
  TEST_ASSERT_EQUAL_INT(0, errno);
  TEST_ASSERT_EQUAL_INT(1024, limit.rlim_cur);

  // setrlimit succeeds (accept-and-ignore; no enforcement). Lowering the soft
  // limit then reading it back must reflect the clamp (soft ≤ hard ≤ MAX_FD).
  struct rlimit nl = {.rlim_cur = 16, .rlim_max = 1024};
  errno = 0;
  TEST_ASSERT_EQUAL_INT(0, setrlimit(RLIMIT_NOFILE, &nl));
  TEST_ASSERT_EQUAL_INT(0, errno);
  struct rlimit got = {0};
  TEST_ASSERT_EQUAL_INT(0, getrlimit(RLIMIT_NOFILE, &got));
  TEST_ASSERT_EQUAL_INT(16, got.rlim_cur);
  // Restore the default.
  struct rlimit def = {.rlim_cur = 1024, .rlim_max = 1024};
  TEST_ASSERT_EQUAL_INT(0, setrlimit(RLIMIT_NOFILE, &def));
}

void test_getrusage_reports_enosys(void) {
  // getrusage (SYS_getrusage) is not yet implemented; the default dispatch
  // returns -ENOSYS.
  struct rusage usage = {0};
  errno = 0;
  TEST_ASSERT_EQUAL_INT(-1, getrusage(RUSAGE_SELF, &usage));
  TEST_ASSERT_EQUAL_INT(ENOSYS, errno);
}

void test_process_priority_round_trips(void) {
  errno = 0;
  int original = getpriority(PRIO_PROCESS, 0);
  TEST_ASSERT_EQUAL_INT(0, errno);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(-20, original);
  TEST_ASSERT_LESS_OR_EQUAL_INT(19, original);

  int updated = original == 5 ? 6 : 5;
  TEST_ASSERT_EQUAL_INT(0, setpriority(PRIO_PROCESS, 0, updated));

  errno = 0;
  TEST_ASSERT_EQUAL_INT(updated, getpriority(PRIO_PROCESS, 0));
  TEST_ASSERT_EQUAL_INT(0, errno);

  TEST_ASSERT_EQUAL_INT(0, setpriority(PRIO_PROCESS, 0, original));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_rlimit_round_trips);
  RUN_TEST(test_getrusage_reports_enosys);
  RUN_TEST(test_process_priority_round_trips);
  return UNITY_END();
}

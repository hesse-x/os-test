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

static void assert_enosys(int result) {
  TEST_ASSERT_EQUAL_INT(-1, result);
  TEST_ASSERT_EQUAL_INT(ENOSYS, errno);
}

void test_unimplemented_resource_syscalls_report_enosys(void) {
  struct rlimit limit = {0};
  struct rusage usage = {0};

  errno = 0;
  assert_enosys(getrlimit(RLIMIT_NOFILE, &limit));

  errno = 0;
  assert_enosys(setrlimit(RLIMIT_NOFILE, &limit));

  errno = 0;
  assert_enosys(getrusage(RUSAGE_SELF, &usage));

  errno = 0;
  assert_enosys(prlimit(0, RLIMIT_NOFILE, NULL, &limit));
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
  RUN_TEST(test_unimplemented_resource_syscalls_report_enosys);
  RUN_TEST(test_process_priority_round_trips);
  return UNITY_END();
}

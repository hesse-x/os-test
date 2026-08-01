/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <sched.h>

#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_sched_getcpu_returns_valid_cpu(void) {
  int cpu = sched_getcpu();

  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, cpu);
  TEST_ASSERT_LESS_THAN_INT(CPU_SETSIZE, cpu);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_sched_getcpu_returns_valid_cpu);
  return UNITY_END();
}

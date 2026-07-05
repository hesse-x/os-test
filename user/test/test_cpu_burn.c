/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// user/test/test_cpu_burn.c — CPU-bound test for work-stealing balance
// verification. Pure compute loop, no syscalls, triggers multi-core load.

#include <stdio.h>

int main(void) {
  volatile unsigned long sum = 0;
  for (unsigned long i = 0; i < 100000000UL; i++) {
    sum += i * 7 + (i & 0xFF);
  }
  printf("cpu_burn done: sum=%lu\n", sum);
  return 0;
}

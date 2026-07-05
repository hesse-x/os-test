/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// user/test/test_cpu_burn.c — CPU 密集型测试,用于 work stealing 均衡验证。
// 纯计算循环,无 syscalls,触发多核负载。

#include <stdio.h>

int main(void) {
  volatile unsigned long sum = 0;
  for (unsigned long i = 0; i < 100000000UL; i++) {
    sum += i * 7 + (i & 0xFF);
  }
  printf("cpu_burn done: sum=%lu\n", sum);
  return 0;
}

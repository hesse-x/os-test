/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// arch/x64/rdrand.c — RDRAND 硬件熵源
//
// CPUID.(EAX=07H,ECX=0):EBX[bit30] = RDRAND 支持位。探测结果在 BSP
// 初始化时缓存（rdrand_init 一次性写入），AP 只读。
// 开发机 i7-4500U（Haswell）有 RDRAND 无 RDSEED（Broadwell+），
// 本驱动不使用 RDSEED。

#include "arch/x64/rdrand.h"
#include "kernel/xcore/log.h"

#define RDRAND_RETRIES 10 // Intel SDM 建议的重试上限

static int has_rdrand;

// BSP 早期调用（xcore_random_init 内），探测并缓存 RDRAND 支持位。
void rdrand_init(void) {
  uint32_t eax = 7, ebx, ecx = 0, edx;
  __asm__ volatile("cpuid"
                   : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                   : "a"(7), "c"(0));
  has_rdrand = !!(ebx & (1u << 30));
  printk(LOG_INFO, "random: rdrand=%d\n", has_rdrand);
}

int rdrand_available(void) { return has_rdrand; }

int rdrand64(uint64_t *out) {
  if (!has_rdrand)
    return -1;
  for (int i = 0; i < RDRAND_RETRIES; i++) {
    uint8_t ok;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(*out), "=qm"(ok));
    if (ok)
      return 0;
  }
  return -1;
}

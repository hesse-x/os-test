/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// arch/x64/rdrand.c — RDRAND hardware entropy source
//
// CPUID.(EAX=07H,ECX=0):EBX[bit30] is the RDRAND support bit. Probed and
// cached by the BSP in rdrand_init; APs only read it. The dev machine
// (i7-4500U, Haswell) has RDRAND but not RDSEED (Broadwell+); this driver
// does not use RDSEED.

#include "arch/x64/rdrand.h"
#include "kernel/xcore/log.h"

#define RDRAND_RETRIES 10 // Intel SDM recommended retry limit

static int has_rdrand;

// Called early on the BSP (xcore_random_init) to probe and cache RDRAND
// support.
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

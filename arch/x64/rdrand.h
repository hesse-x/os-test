/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ARCH_X64_RDRAND_H
#define ARCH_X64_RDRAND_H

#include <stdint.h>

void rdrand_init(void);      // early BSP: CPUID probe and cache
int rdrand_available(void);  // cached probe result (set by BSP init)
int rdrand64(uint64_t *out); // 0 on success / -1 on retry exhaustion

#endif // ARCH_X64_RDRAND_H

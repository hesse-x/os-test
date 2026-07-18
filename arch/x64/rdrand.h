/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ARCH_X64_RDRAND_H
#define ARCH_X64_RDRAND_H

#include <stdint.h>

void rdrand_init(void);      // BSP 早期调用：CPUID 探测并缓存
int rdrand_available(void);  // CPUID 探测结果（BSP 初始化时缓存）
int rdrand64(uint64_t *out); // 0 成功 / -1 重试耗尽

#endif // ARCH_X64_RDRAND_H

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_XCORE_RANDOM_H
#define KERNEL_XCORE_RANDOM_H

#include <stddef.h>

void xcore_random_init(void); // BSP 早期调用（含 ChaCha20 自检）
void csprng_read(void *buf, size_t len); // 内核 buf，永不失败/阻塞

#endif // KERNEL_XCORE_RANDOM_H

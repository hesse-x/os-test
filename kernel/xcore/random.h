/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_XCORE_RANDOM_H
#define KERNEL_XCORE_RANDOM_H

#include <stddef.h>

void xcore_random_init(void); // BSP early call (includes ChaCha20 self-test)
void csprng_read(void *buf, size_t len); // kernel buffer, never fails/blocks

#endif // KERNEL_XCORE_RANDOM_H

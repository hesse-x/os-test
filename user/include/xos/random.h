/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
/* Non-standard random APIs implemented by the XOS libc. */
#ifndef XOS_RANDOM_H
#define XOS_RANDOM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void arc4random_buf(void *, size_t);
uint32_t arc4random_uniform(uint32_t);

#ifdef __cplusplus
}
#endif

#endif

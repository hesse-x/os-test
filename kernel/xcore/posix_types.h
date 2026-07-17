/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef KERNEL_XCORE_POSIX_TYPES_H
#define KERNEL_XCORE_POSIX_TYPES_H

#include <stdint.h>

/* ssize_t is POSIX (<sys/types.h>), not C standard — absent from the
 * freestanding kernel. Singular canonical definition so every layer
 * (xcore/bsd/driver) pulls it from one place instead of re-typedef'ing. */
typedef int64_t ssize_t;

#endif /* KERNEL_XCORE_POSIX_TYPES_H */

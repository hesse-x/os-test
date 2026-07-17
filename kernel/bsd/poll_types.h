/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef KERNEL_BSD_POLL_TYPES_H
#define KERNEL_BSD_POLL_TYPES_H

#include <stdint.h>

/* Kernel-internal poll event bitmask type. The matching POLLIN/POLLOUT/...
 * constants live in <xos/socket.h>; this is the singular canonical home for
 * the type itself, so consumers need not drag in the heavier file_operations
 * header just to spell a poll callback's return type. */
typedef uint32_t __poll;

#endif /* KERNEL_BSD_POLL_TYPES_H */

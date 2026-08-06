/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef XOS_KERNEL_PERF_UNWIND_H
#define XOS_KERNEL_PERF_UNWIND_H

#include <stdint.h>

#include "arch/x64/trap.h"

#ifdef PERF
uint8_t perf_unwind_trapframe(const trapframe *tf, uint64_t *frames,
                              uint8_t *stop_reason);
#endif

#endif

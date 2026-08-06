/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef XOS_KERNEL_PERF_PMU_H
#define XOS_KERNEL_PERF_PMU_H

#include <stdbool.h>
#include <stdint.h>

#include "arch/x64/trap.h"

#ifdef PERF
void perf_pmu_ensure_cpu(void);
bool perf_pmu_active_cpu(void);
void perf_pmu_handle_nmi(const trapframe *tf);
void perf_pmu_stop_cpu(void);
uint64_t perf_pmu_nmi_count(void);
uint64_t perf_pmu_handler_cycles(void);
uint32_t perf_pmu_active_mask(void);
#else
static inline void perf_pmu_ensure_cpu(void) {}
static inline bool perf_pmu_active_cpu(void) { return false; }
static inline void perf_pmu_handle_nmi(const trapframe *tf) { (void)tf; }
static inline void perf_pmu_stop_cpu(void) {}
#endif

#endif

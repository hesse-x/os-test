/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/xcore/perf/pmu.h"

#ifdef PERF

#include <xos/perf.h>

#include "arch/x64/apic.h"
#include "arch/x64/smp.h"
#include "arch/x64/utils.h"
#include "kernel/xcore/perf/sample.h"
#include "kernel/xcore/perf/unwind.h"

#define MSR_IA32_PMC0 0x0c1U
#define MSR_IA32_PERFEVTSEL0 0x186U
#define MSR_IA32_PERF_GLOBAL_STATUS 0x38eU
#define MSR_IA32_PERF_GLOBAL_CTRL 0x38fU
#define MSR_IA32_PERF_GLOBAL_OVF_CTRL 0x390U

#define PERF_EVENT_CPU_CYCLES 0x3cU
#define PERF_EVENTSEL_USR (1ULL << 16)
#define PERF_EVENTSEL_OS (1ULL << 17)
#define PERF_EVENTSEL_INT (1ULL << 20)
#define PERF_EVENTSEL_ENABLE (1ULL << 22)

// 0=uninitialized, 1=PMU NMI, 2=timer fallback.
static uint8_t pmu_state[MAX_CPUS];
static uint8_t pmu_width[MAX_CPUS];
static uint64_t pmu_period[MAX_CPUS];
static uint32_t pmu_jitter[MAX_CPUS];
static uint64_t pmu_nmis[MAX_CPUS];
static uint64_t pmu_cycles[MAX_CPUS];
static uint64_t pmu_rate_window[MAX_CPUS];
static uint32_t pmu_rate_count[MAX_CPUS];

static void cpuid_count(uint32_t leaf, uint32_t subleaf, uint32_t *a,
                        uint32_t *b, uint32_t *c, uint32_t *d) {
  __asm__ volatile("cpuid"
                   : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                   : "a"(leaf), "c"(subleaf));
}

static uint64_t counter_mask(unsigned cpu) {
  return pmu_width[cpu] == 64 ? ~0ULL : (1ULL << pmu_width[cpu]) - 1ULL;
}

static void rearm(unsigned cpu) {
  uint32_t random = pmu_jitter[cpu] * 1664525U + 1013904223U;
  pmu_jitter[cpu] = random;
  int32_t delta = (int32_t)(random % 129U) - 64;
  uint64_t period = pmu_period[cpu] + (pmu_period[cpu] * (int64_t)delta) / 1024;
  wrmsr(MSR_IA32_PMC0, (0ULL - period) & counter_mask(cpu));
}

void perf_pmu_ensure_cpu(void) {
  unsigned cpu = (unsigned)get_cpu_local()->cpu_id;
  if (cpu >= MAX_CPUS || pmu_state[cpu] != 0)
    return;
  uint32_t a, b, c, d;
  cpuid_count(0, 0, &a, &b, &c, &d);
  if (a < 0x0aU) {
    pmu_state[cpu] = 2;
    return;
  }
  cpuid_count(0x0a, 0, &a, &b, &c, &d);
  unsigned version = a & 0xffU;
  unsigned counters = (a >> 8) & 0xffU;
  unsigned width = (a >> 16) & 0xffU;
  if (version < 2 || counters == 0 || width < 32 || width > 64 || !tsc_freq) {
    pmu_state[cpu] = 2;
    return;
  }

  pmu_width[cpu] = (uint8_t)width;
  pmu_period[cpu] = tsc_freq / 1000U;
  if (pmu_period[cpu] < 10000U)
    pmu_period[cpu] = 10000U;
  pmu_jitter[cpu] = 0x9e3779b9U ^ (cpu * 0x85ebca6bU);
  wrmsr(MSR_IA32_PERF_GLOBAL_CTRL, 0);
  wrmsr(MSR_IA32_PERFEVTSEL0, 0);
  wrmsr(MSR_IA32_PERF_GLOBAL_OVF_CTRL, 1);
  lapic_write(LAPIC_LVT_PERFMON, LAPIC_LVT_DELIVERY_NMI);
  rearm(cpu);
  wrmsr(MSR_IA32_PERFEVTSEL0, PERF_EVENT_CPU_CYCLES | PERF_EVENTSEL_USR |
                                  PERF_EVENTSEL_OS | PERF_EVENTSEL_INT |
                                  PERF_EVENTSEL_ENABLE);
  pmu_state[cpu] = 1;
  wrmsr(MSR_IA32_PERF_GLOBAL_CTRL, 1);
}

bool perf_pmu_active_cpu(void) {
  unsigned cpu = (unsigned)get_cpu_local()->cpu_id;
  return cpu < MAX_CPUS && pmu_state[cpu] == 1;
}

void perf_pmu_handle_nmi(const trapframe *tf) {
  unsigned cpu = (unsigned)get_cpu_local()->cpu_id;
  if (cpu >= MAX_CPUS || pmu_state[cpu] != 1)
    return;
  uint64_t status = rdmsr(MSR_IA32_PERF_GLOBAL_STATUS);
  if (!(status & 1U))
    return;
  uint64_t start = rdtsc64();
  if (start - pmu_rate_window[cpu] >= tsc_freq) {
    pmu_rate_window[cpu] = start;
    pmu_rate_count[cpu] = 0;
  }
  if (++pmu_rate_count[cpu] > 5000U) {
    wrmsr(MSR_IA32_PERF_GLOBAL_CTRL, 0);
    lapic_write(LAPIC_LVT_PERFMON, LAPIC_LVT_MASKED);
    pmu_state[cpu] = 2;
    return;
  }
  wrmsr(MSR_IA32_PERF_GLOBAL_OVF_CTRL, 1);
  uint64_t frames[PERF_CALLCHAIN_MAX_DEPTH];
  uint8_t stop;
  uint8_t depth = perf_unwind_trapframe(tf, frames, &stop);
  perf_record_callchain(cpu, frames, depth, XOS_PERF_SAMPLE_PMU_NMI, stop);
  pmu_nmis[cpu]++;
  rearm(cpu);
  pmu_cycles[cpu] += rdtsc64() - start;
}

void perf_pmu_stop_cpu(void) {
  unsigned cpu = (unsigned)get_cpu_local()->cpu_id;
  if (cpu < MAX_CPUS && pmu_state[cpu] == 1) {
    wrmsr(MSR_IA32_PERF_GLOBAL_CTRL, 0);
    lapic_write(LAPIC_LVT_PERFMON, LAPIC_LVT_MASKED);
  }
}

uint64_t perf_pmu_nmi_count(void) {
  uint64_t total = 0;
  for (unsigned cpu = 0; cpu < MAX_CPUS; cpu++)
    total += __atomic_load_n(&pmu_nmis[cpu], __ATOMIC_RELAXED);
  return total;
}

uint64_t perf_pmu_handler_cycles(void) {
  uint64_t total = 0;
  for (unsigned cpu = 0; cpu < MAX_CPUS; cpu++)
    total += __atomic_load_n(&pmu_cycles[cpu], __ATOMIC_RELAXED);
  return total;
}

uint32_t perf_pmu_active_mask(void) {
  uint32_t mask = 0;
  for (unsigned cpu = 0; cpu < MAX_CPUS; cpu++)
    if (__atomic_load_n(&pmu_state[cpu], __ATOMIC_ACQUIRE) == 1)
      mask |= 1U << cpu;
  return mask;
}

#endif

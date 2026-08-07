/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// kernel/xcore/init.c — Xcore initialization sequence
// Extracted from kernel/kernel.c (phase 5 step 5.1)

#include "arch/x64/paging.h"
#include "arch/x64/smp.h"
#include "arch/x64/utils.h"
#include "boot/boot.h"
#include "kernel/kernel.h"
#include "kernel/xcore/acpi.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/mem/kasan.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/perf/phase.h"
#include "kernel/xcore/random.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/serial_hook.h"
#include "kernel/xcore/trap.h"

#ifdef PERF
#include "kernel/xcore/perf/phase_ids.h"
#endif
__attribute__((no_sanitize("kernel-address"))) void xcore_init(boot_info *bi) {
  serial_init();

  if (bi->magic != BOOT_INFO_MAGIC) {
    printk(LOG_ERROR, "xcore_init: bad boot_info magic!\n");
    halt();
  }

  PERF_PHASE_BEGIN(PERF_PHASE_MEMORY);
  init_mem(bi);
  PERF_PHASE_END(PERF_PHASE_MEMORY);
  PERF_PHASE_BEGIN(PERF_PHASE_ACPI);
  acpi_init(bi->rsdp);
  PERF_PHASE_END(PERF_PHASE_ACPI);

  // init_mem is the bump allocator's only post-paging consumer. Finalize its
  // pages before KASAN starts consuming BFC pages for shadow page tables.
  bump_disable();

  kasan_init();
  slab_init();

  PERF_PHASE_BEGIN(PERF_PHASE_APIC_TSC);
  irq_init();
  PERF_PHASE_END(PERF_PHASE_APIC_TSC);

  // rcu_init();  // RCU is initialized lazily in sched_init

  sig_init(); // allocate signal trampoline page (shared across all processes)
  PERF_PHASE_BEGIN(PERF_PHASE_SCHEDULER);
  sched_init(); // initialize process table + cpu_locals
  PERF_PHASE_END(PERF_PHASE_SCHEDULER);

  xcore_random_init(); // RDRAND probe + ChaCha20 self-test + seed CPU0

  PERF_PHASE_BEGIN(PERF_PHASE_SMP);
  smp_boot_aps();
  PERF_PHASE_END(PERF_PHASE_SMP);

  printk(LOG_INFO, "xcore_init: done\n");
}

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
#include "utils/macro.h"
#include "kernel/kernel.h"
#include "kernel/xcore/acpi.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/mem/kasan.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/rcu.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/serial_hook.h"
#include "kernel/xcore/trap.h"
#include "kernel/xcore/xtask.h"

__attribute__((no_sanitize("kernel-address"))) void xcore_init(boot_info *bi) {
  serial_init();

  if (bi->magic != BOOT_INFO_MAGIC) {
    printk(LOG_ERROR, "xcore_init: bad boot_info magic!\n");
    halt();
  }

  init_mem(bi);
  acpi_init(bi->rsdp);
  irq_init();

  // Disable bump allocator
  bump_disable();

  kasan_init();
  slab_init();

  // rcu_init();  // RCU is initialized lazily in sched_init

  sig_init();  // allocate signal trampoline page (shared across all processes)
  sched_init(); // initialize process table + cpu_locals

  smp_boot_aps();

  printk(LOG_INFO, "xcore_init: done\n");
}

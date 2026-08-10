/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// kernel/driver/init.c — Driver initialization sequence
// Extracted from kernel/kernel.c (phase 5 step 5.2)

#include "kernel/driver/driver.h"
#include "kernel/driver/drm/drm_core.h"
#include "kernel/driver/intel/i915_drv.h"
#include "kernel/driver/intel/i915_probe.h"
#include "kernel/driver/pci.h"
#include "kernel/driver/virtio_gpu.h"
#include "kernel/driver/xhci.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/perf/phase.h"
#include "kernel/xcore/trap.h"

#ifdef PERF
#include "kernel/xcore/perf/phase_ids.h"
#endif
// Driver definitions (in respective .c files)
extern dev_driver ahci_driver;
extern dev_driver xhci_driver;
extern dev_driver serial_driver;

static void driver_timer_poll(void) {
  xhci_poll();
  virtio_gpu_poll();
}

void driver_init(void) {
  PERF_PHASE_BEGIN(PERF_PHASE_PCI);
  pci_init();
  drm_core_init();
  PERF_PHASE_END(PERF_PHASE_PCI);
  printk(LOG_INFO, "driver_init: pci_init done\n");

  // Register all built-in drivers
  driver_register(&ahci_driver);
  driver_register(&xhci_driver);
  if (virtio_gpu_register_driver() != 0)
    printk(LOG_ERROR,
           "driver_init: virtio-gpu PCI driver registration failed\n");
  if (i915_probe_register() != 0)
    printk(LOG_ERROR, "driver_init: i915 probe-only registration failed\n");
  driver_register(&serial_driver);

  // PCI class/vendor auto-match: calls init() for matched drivers
  driver_pci_match();
  pci_lifecycle_selftest();
  i915_probe_selftest();
  drm_mock_register_test_device();

  // Poll drivers that need coarse periodic service (currently every ~10 ms).
  timer_poll_hook = driver_timer_poll;

  printk(LOG_INFO, "driver_init: done\n");
}

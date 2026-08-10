/*
 * Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */

#include "arch/x64/apic.h"
#include "arch/x64/trap.h"
#include "kernel/driver/intel/i915_drv.h"
#include "kernel/driver/intel/i915_regs.h"
#include "kernel/driver/pci.h"
#include "kernel/xcore/atomic.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <xos/errno.h>

#define I915_IRQ_STORM_THRESHOLD 64

static const uint32_t i915_irq_source_enable[] = {
    I915_GEN11_RENDER_COPY_INTR_ENABLE, I915_GEN11_VCS_VECS_INTR_ENABLE,
    I915_GEN11_GUC_SG_INTR_ENABLE,      I915_GEN11_GPM_WGBOXPERF_INTR_ENABLE,
    I915_GEN11_CRYPTO_RSVD_INTR_ENABLE, I915_GEN11_GUNIT_CSME_INTR_ENABLE,
    I915_GEN12_CCS_RSVD_INTR_ENABLE,
};

static int i915_irq_quiesce(struct i915_device *i915) {
  int rc = i915_mmio_write32_allowed(i915, I915_GEN11_GFX_MSTR_IRQ, 0,
                                     I915_WRITE_IRQ_CONTROL);
  for (size_t i = 0; !rc && i < sizeof(i915_irq_source_enable) /
                                    sizeof(i915_irq_source_enable[0]);
       i++) {
    rc = i915_mmio_write32_allowed(i915, i915_irq_source_enable[i], 0,
                                   I915_WRITE_IRQ_CONTROL);
    uint32_t masked;
    if (!rc)
      rc = i915_posting_read32(i915, i915_irq_source_enable[i], &masked);
    if (!rc && masked != 0)
      rc = -EIO;
  }
  uint32_t posted;
  if (!rc)
    rc = i915_posting_read32(i915, I915_GEN11_GFX_MSTR_IRQ, &posted);
  return rc;
}

static void i915_irq_process(struct i915_device *i915) {
  atomic_inc(&i915->irq_count);
  uint32_t pending = 0;
  if (i915->quiescing ||
      i915_mmio_read32(i915, I915_GEN11_GFX_MSTR_IRQ, &pending)) {
    atomic_inc(&i915->irq_spurious);
    return;
  }
  if (!(pending & ~I915_GEN11_MASTER_IRQ)) {
    if (atomic_inc_return(&i915->irq_spurious) >= I915_IRQ_STORM_THRESHOLD) {
      (void)i915_mmio_write32_allowed(i915, I915_GEN11_GFX_MSTR_IRQ, 0,
                                      I915_WRITE_IRQ_CONTROL);
      i915->degraded = true;
    }
  } else {
    atomic_inc(&i915->irq_unknown);
    (void)i915_mmio_write32_allowed(i915, I915_GEN11_GFX_MSTR_IRQ, pending,
                                    I915_WRITE_IRQ_CONTROL);
    (void)i915_mmio_write32_allowed(i915, I915_GEN11_GFX_MSTR_IRQ, 0,
                                    I915_WRITE_IRQ_CONTROL);
    i915->degraded = true;
  }
}

static void i915_irq_handler(trapframe *frame, void *ctx) {
  (void)frame;
  i915_irq_process(ctx);
  lapic_eoi();
}

#ifdef TEST
void i915_irq_test_invoke(struct i915_device *i915) { i915_irq_process(i915); }
#endif

int i915_irq_install(struct i915_device *i915) {
  if (!i915)
    return -EINVAL;
  int rc = i915_irq_quiesce(i915);
  if (rc)
    return rc;
#ifdef TEST
  if (i915->test_irq_install) {
    rc = i915->test_irq_install(i915);
    if (!rc)
      i915->resources |= I915_RES_IRQ;
    return rc;
  }
#endif
  rc = pci_enable_msi(i915->pdev);
  if (rc)
    return rc;
  rc = pci_request_irq_ctx(i915->pdev, 0, i915_irq_handler, i915);
  if (rc) {
    pci_disable_interrupts(i915->pdev);
    return rc;
  }
  i915->resources |= I915_RES_IRQ;
  return 0;
}

void i915_irq_uninstall(struct i915_device *i915) {
  if (!i915 || !(i915->resources & I915_RES_IRQ))
    return;
  (void)i915_irq_quiesce(i915);
#ifdef TEST
  if (i915->test_irq_uninstall) {
    i915->test_irq_uninstall(i915);
    i915->resources &= ~I915_RES_IRQ;
    return;
  }
#endif
  pci_free_irq(i915->pdev, 0);
  pci_disable_interrupts(i915->pdev);
  i915->resources &= ~I915_RES_IRQ;
}

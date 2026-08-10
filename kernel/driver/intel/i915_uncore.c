/*
 * Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */

#include "arch/x64/apic.h"
#include "arch/x64/utils.h"
#include "kernel/driver/intel/i915_drv.h"
#include "kernel/driver/intel/i915_regs.h"
#include "kernel/xcore/mutex.h"
#include "kernel/xcore/sparse.h"
#include <stdbool.h>
#include <stdint.h>
#include <xos/errno.h>

#define I915_FORCEWAKE_DOMAIN_GT 1u
#define I915_FORCEWAKE_TIMEOUT_US 1000u

static int i915_mmio_check(const struct i915_device *i915, uint32_t offset,
                           uint32_t width) {
  if (!i915 || !i915->regs || !width || offset % width)
    return -EINVAL;
  if (offset > i915->regs_size || width > i915->regs_size - offset)
    return -ERANGE;
  return 0;
}

int i915_mmio_read32(struct i915_device *i915, uint32_t offset,
                     uint32_t *value) {
  if (!value)
    return -EINVAL;
  int rc = i915_mmio_check(i915, offset, sizeof(uint32_t));
  if (rc)
    return rc;
#ifdef TEST
  if (i915->test_mmio_read)
    return i915->test_mmio_read(i915, offset, value);
#endif
  volatile uint32_t __iomem *reg =
      (volatile uint32_t __iomem *)((uint8_t __iomem *)i915->regs + offset);
  *value = *(volatile uint32_t __force *)reg;
  __asm__ volatile("" ::: "memory");
  return 0;
}

int i915_mmio_read64(struct i915_device *i915, uint32_t offset,
                     uint64_t *value) {
  if (!value)
    return -EINVAL;
  int rc = i915_mmio_check(i915, offset, sizeof(uint64_t));
  if (rc)
    return rc;
  uint32_t low, high;
  rc = i915_mmio_read32(i915, offset, &low);
  if (!rc)
    rc = i915_mmio_read32(i915, offset + 4, &high);
  if (!rc)
    *value = ((uint64_t)high << 32) | low;
  return rc;
}

#if defined(TEST) || I915_PROBE_STAGE >= I915_PROBE_STAGE_FORCEWAKE
static bool i915_write_allowed(uint32_t offset, enum i915_write_reason reason) {
  if (reason == I915_WRITE_FORCEWAKE)
    return offset == I915_FORCEWAKE_REQ_GT;
#if defined(TEST) || I915_PROBE_STAGE >= I915_PROBE_STAGE_IRQ_SAFE
  if (reason != I915_WRITE_IRQ_CONTROL)
    return false;
  return offset == I915_GEN11_GFX_MSTR_IRQ ||
         offset == I915_GEN11_RENDER_COPY_INTR_ENABLE ||
         offset == I915_GEN11_VCS_VECS_INTR_ENABLE ||
         offset == I915_GEN11_GUC_SG_INTR_ENABLE ||
         offset == I915_GEN11_GPM_WGBOXPERF_INTR_ENABLE ||
         offset == I915_GEN11_CRYPTO_RSVD_INTR_ENABLE ||
         offset == I915_GEN11_GUNIT_CSME_INTR_ENABLE ||
         offset == I915_GEN12_CCS_RSVD_INTR_ENABLE;
#else
  return false;
#endif
}

int i915_mmio_write32_allowed(struct i915_device *i915, uint32_t offset,
                              uint32_t value, enum i915_write_reason reason) {
  int rc = i915_mmio_check(i915, offset, sizeof(uint32_t));
  if (rc)
    return rc;
  if (!i915_write_allowed(offset, reason))
    return -EPERM;
#ifdef TEST
  if (i915->test_mmio_write)
    return i915->test_mmio_write(i915, offset, value);
#endif
  volatile uint32_t __iomem *reg =
      (volatile uint32_t __iomem *)((uint8_t __iomem *)i915->regs + offset);
  *(volatile uint32_t __force *)reg = value;
  __asm__ volatile("" ::: "memory");
  return 0;
}
#endif

int i915_posting_read32(struct i915_device *i915, uint32_t offset,
                        uint32_t *value) {
  return i915_mmio_read32(i915, offset, value);
}

#if defined(TEST) || I915_PROBE_STAGE >= I915_PROBE_STAGE_FORCEWAKE
static int i915_wait_forcewake(struct i915_device *i915, bool asserted) {
  uint64_t budget =
      tsc_freq ? (tsc_freq / 1000000u) * I915_FORCEWAKE_TIMEOUT_US : 1000000u;
  uint64_t start = rdtsc64();
  uint32_t ack = 0;
  do {
    int rc = i915_mmio_read32(i915, I915_FORCEWAKE_ACK_GT, &ack);
    if (rc)
      return rc;
    i915->last_forcewake_ack = ack;
    if (!!(ack & I915_FORCEWAKE_KERNEL) == asserted)
      return 0;
    __asm__ volatile("pause" ::: "memory");
  } while (rdtsc64() - start < budget);
  return -ETIMEDOUT;
}

int i915_forcewake_get(struct i915_device *i915, uint32_t domains) {
  if (!i915 || domains != I915_FORCEWAKE_DOMAIN_GT || i915->quiescing)
    return -EINVAL;
  mutex_lock(&i915->uncore_mutex);
  if (i915->forcewake_ref++ != 0) {
    mutex_unlock(&i915->uncore_mutex);
    return 0;
  }
  int rc = i915_mmio_write32_allowed(
      i915, I915_FORCEWAKE_REQ_GT, I915_FORCEWAKE_ENABLE, I915_WRITE_FORCEWAKE);
  if (!rc)
    rc = i915_wait_forcewake(i915, true);
  if (rc) {
    i915->forcewake_ref = 0;
    i915->degraded = true;
    (void)i915_mmio_write32_allowed(i915, I915_FORCEWAKE_REQ_GT,
                                    I915_FORCEWAKE_DISABLE,
                                    I915_WRITE_FORCEWAKE);
  }
  mutex_unlock(&i915->uncore_mutex);
  return rc;
}

void i915_forcewake_put(struct i915_device *i915, uint32_t domains) {
  if (!i915 || domains != I915_FORCEWAKE_DOMAIN_GT)
    return;
  mutex_lock(&i915->uncore_mutex);
  if (i915->forcewake_ref == 0) {
    i915->degraded = true;
    mutex_unlock(&i915->uncore_mutex);
    return;
  }
  if (--i915->forcewake_ref == 0) {
    int rc =
        i915_mmio_write32_allowed(i915, I915_FORCEWAKE_REQ_GT,
                                  I915_FORCEWAKE_DISABLE, I915_WRITE_FORCEWAKE);
    if (!rc)
      rc = i915_wait_forcewake(i915, false);
    if (rc)
      i915->degraded = true;
  }
  mutex_unlock(&i915->uncore_mutex);
}
#endif

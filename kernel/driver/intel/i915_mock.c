/*
 * Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */

#include "kernel/driver/intel/i915_drv.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/mutex.h"
#include "kernel/xcore/sparse.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef TEST

#include "kernel/driver/intel/i915_regs.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/alloc.h"
#include <xos/errno.h>

#define I915_FORCEWAKE_DOMAIN_GT 1u

struct i915_mock_mmio {
  uint32_t forcewake_request;
  uint32_t forcewake_ack;
  uint32_t master_irq;
  unsigned int ack_after_reads;
  unsigned int ack_reads;
  bool never_ack;
  uint32_t last_write_offset;
  uint32_t last_write_value;
};

static int i915_mock_read(struct i915_device *i915, uint32_t offset,
                          uint32_t *value) {
  struct i915_mock_mmio *mmio = i915->test_private;
  if (offset == I915_FORCEWAKE_ACK_GT && !mmio->never_ack &&
      mmio->ack_reads++ >= mmio->ack_after_reads) {
    mmio->forcewake_ack = (mmio->forcewake_request & I915_FORCEWAKE_KERNEL)
                              ? I915_FORCEWAKE_KERNEL
                              : 0;
  }
  if (offset == I915_FORCEWAKE_ACK_GT)
    *value = mmio->forcewake_ack;
  else if (offset == I915_GEN11_GFX_MSTR_IRQ)
    *value = mmio->master_irq;
  else
    *value = 0;
  return 0;
}

static int i915_mock_write(struct i915_device *i915, uint32_t offset,
                           uint32_t value) {
  struct i915_mock_mmio *mmio = i915->test_private;
  mmio->last_write_offset = offset;
  mmio->last_write_value = value;
  if (offset == I915_FORCEWAKE_REQ_GT) {
    mmio->forcewake_request = value;
    mmio->ack_reads = 0;
  } else if (offset == I915_GEN11_GFX_MSTR_IRQ) {
    mmio->master_irq = value;
  }
  return 0;
}

static void i915_mock_init(struct i915_device *i915,
                           struct i915_mock_mmio *mmio) {
  *i915 = (struct i915_device){
      .regs = (void __iomem *)mmio,
      .regs_size = 2u * 1024u * 1024u,
      .test_mmio_read = i915_mock_read,
      .test_mmio_write = i915_mock_write,
      .test_private = mmio,
  };
  mutex_init(&i915->uncore_mutex);
  atomic_set(&i915->irq_count, 0);
  atomic_set(&i915->irq_spurious, 0);
  atomic_set(&i915->irq_unknown, 0);
}

static void i915_mock_forcewake_test(void) {
  struct i915_mock_mmio mmio = {0};
  struct i915_device i915;
  i915_mock_init(&i915, &mmio);

  BUG_ON(i915_forcewake_get(&i915, I915_FORCEWAKE_DOMAIN_GT));
  i915_forcewake_put(&i915, I915_FORCEWAKE_DOMAIN_GT);
  BUG_ON(i915.forcewake_ref || i915.degraded);

  mmio = (struct i915_mock_mmio){.ack_after_reads = 3};
  i915_mock_init(&i915, &mmio);

  BUG_ON(i915_forcewake_get(&i915, I915_FORCEWAKE_DOMAIN_GT));
  BUG_ON(i915.forcewake_ref != 1 || mmio.ack_reads < 4);
  BUG_ON(i915_forcewake_get(&i915, I915_FORCEWAKE_DOMAIN_GT));
  BUG_ON(i915.forcewake_ref != 2);
  i915_forcewake_put(&i915, I915_FORCEWAKE_DOMAIN_GT);
  BUG_ON(i915.forcewake_ref != 1);
  i915_forcewake_put(&i915, I915_FORCEWAKE_DOMAIN_GT);
  BUG_ON(i915.forcewake_ref || i915.degraded || i915.last_forcewake_ack);

  mmio.never_ack = true;
  BUG_ON(i915_forcewake_get(&i915, I915_FORCEWAKE_DOMAIN_GT) != -ETIMEDOUT);
  BUG_ON(i915.forcewake_ref || !i915.degraded);

  i915.degraded = false;
  i915_forcewake_put(&i915, I915_FORCEWAKE_DOMAIN_GT);
  BUG_ON(!i915.degraded);
}

static void i915_mock_irq_test(void) {
  struct i915_mock_mmio mmio = {0};
  struct i915_device i915;
  i915_mock_init(&i915, &mmio);

  for (int i = 0; i < 63; i++)
    i915_irq_test_invoke(&i915);
  BUG_ON(i915.degraded || atomic_read(&i915.irq_spurious) != 63);
  i915_irq_test_invoke(&i915);
  BUG_ON(!i915.degraded || atomic_read(&i915.irq_spurious) != 64);
  BUG_ON(mmio.last_write_offset != I915_GEN11_GFX_MSTR_IRQ ||
         mmio.last_write_value != 0);

  i915.degraded = false;
  mmio.master_irq = 1u;
  i915_irq_test_invoke(&i915);
  BUG_ON(!i915.degraded || atomic_read(&i915.irq_unknown) != 1);
}

static void i915_mock_ownership_test(void) {
  uint8_t statuses[4] = {PAGE_RESERVED, PAGE_RESERVED, PAGE_RESERVED,
                         PAGE_RESERVED};
  size_t conflict = SIZE_MAX;
  BUG_ON(i915_stolen_test_ownership(statuses, 4, 0, 4, &conflict));
  statuses[2] = PAGE_FREE;
  BUG_ON(i915_stolen_test_ownership(statuses, 4, 0, 4, &conflict) != -EBUSY);
  BUG_ON(conflict != 2);
  statuses[2] = PAGE_USED;
  BUG_ON(i915_stolen_test_ownership(statuses, 4, 1, 3, &conflict) != -EBUSY);
  statuses[2] = PAGE_SLAB;
  BUG_ON(i915_stolen_test_ownership(statuses, 4, 0, 4, &conflict) != -EBUSY);
  BUG_ON(i915_stolen_test_ownership(statuses, 4, 3, 2, &conflict) != -ERANGE);
}

static void i915_mock_snapshot_test(void) {
  struct i915_device i915 = {
      .state = I915_FORCEWAKE_READY,
      .resources = I915_RES_PRIVATE | I915_RES_BAR0,
      .degraded = true,
      .forcewake_ref = 2,
      .last_forcewake_ack = 1,
  };
  atomic_set(&i915.irq_count, 7);
  atomic_set(&i915.irq_spurious, 3);
  atomic_set(&i915.irq_unknown, 1);
  struct i915_debug_snapshot snapshot = {0};
  i915_debug_snapshot(&i915, &snapshot);
  BUG_ON(snapshot.state != i915.state || snapshot.resources != i915.resources ||
         !snapshot.degraded || snapshot.forcewake_ref != 2 ||
         snapshot.last_forcewake_ack != 1 || snapshot.irq_count != 7 ||
         snapshot.irq_spurious != 3 || snapshot.irq_unknown != 1);
}

void i915_mock_selftest(void) {
  i915_mock_forcewake_test();
  i915_mock_irq_test();
  i915_mock_ownership_test();
  i915_mock_snapshot_test();
  printk(
      LOG_INFO,
      "i915 mock selftest: PASS (forcewake, IRQ storm, ownership, snapshot)\n");
}

#else

typedef int i915_mock_translation_unit_not_empty;

#endif

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef KERNEL_DRIVER_INTEL_I915_DRV_H
#define KERNEL_DRIVER_INTEL_I915_DRV_H

#include "kernel/driver/pci.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/mutex.h"
#include "kernel/xcore/spinlock.h"
#include <stdbool.h>
#include <stdint.h>

enum i915_probe_state {
  I915_ALLOCATED,
  I915_MATCHED,
  I915_MMIO_MAPPED,
  I915_PLATFORM_PARSED,
  I915_FORCEWAKE_READY,
  I915_IRQ_READY,
  I915_PROBED,
  I915_QUIESCING,
  I915_FAILED,
  I915_DEAD,
};

enum i915_resource {
  I915_RES_PRIVATE = 1u << 0,
  I915_RES_COMMAND = 1u << 1,
  I915_RES_BAR0 = 1u << 2,
  I915_RES_FORCEWAKE = 1u << 3,
  I915_RES_IRQ = 1u << 4,
};

enum i915_write_reason {
  I915_WRITE_FORCEWAKE,
  I915_WRITE_IRQ_CONTROL,
};

struct i915_platform_info {
  uint16_t device_id;
  uint8_t revision_id;
  uint8_t graphics_ver;
  uint8_t dma_mask_bits;
  uint32_t engine_mask;
};

struct i915_memory_layout {
  uint16_t gmch_ctrl;
  uint8_t gms;
  uint8_t ggms;
  uint64_t stolen_base;
  uint64_t stolen_size;
  uint64_t ggtt_size;
  uint64_t aperture_size;
};

struct i915_debug_snapshot {
  enum i915_probe_state state;
  uint32_t resources;
  bool quiescing;
  bool degraded;
  uint32_t forcewake_ref;
  uint32_t last_forcewake_ack;
  int irq_count;
  int irq_spurious;
  int irq_unknown;
};

#ifdef TEST
struct i915_device;
typedef int (*i915_test_mmio_read_fn)(struct i915_device *i915, uint32_t offset,
                                      uint32_t *value);
typedef int (*i915_test_mmio_write_fn)(struct i915_device *i915,
                                       uint32_t offset, uint32_t value);
typedef int (*i915_test_irq_install_fn)(struct i915_device *i915);
typedef void (*i915_test_irq_uninstall_fn)(struct i915_device *i915);
#endif

struct i915_device {
  pci_device *pdev;
  const struct i915_platform_info *info;
  void __iomem *regs;
  uint64_t regs_size;
  uint16_t original_command;
  enum i915_probe_state state;
  uint32_t resources;
  bool quiescing;
  bool degraded;
  mutex lifecycle_mutex;
  mutex uncore_mutex;
  spinlock forcewake_lock;
  spinlock irq_lock;
  uint32_t forcewake_ref;
  uint32_t last_forcewake_ack;
  struct i915_memory_layout memory;
  atomic_t irq_count;
  atomic_t irq_spurious;
  atomic_t irq_unknown;
#ifdef TEST
  i915_test_mmio_read_fn test_mmio_read;
  i915_test_mmio_write_fn test_mmio_write;
  i915_test_irq_install_fn test_irq_install;
  i915_test_irq_uninstall_fn test_irq_uninstall;
  void *test_private;
#endif
};

int i915_mmio_read32(struct i915_device *i915, uint32_t offset,
                     uint32_t *value);
int i915_mmio_read64(struct i915_device *i915, uint32_t offset,
                     uint64_t *value);
int i915_mmio_write32_allowed(struct i915_device *i915, uint32_t offset,
                              uint32_t value, enum i915_write_reason reason);
int i915_posting_read32(struct i915_device *i915, uint32_t offset,
                        uint32_t *value);
int i915_forcewake_get(struct i915_device *i915, uint32_t domains);
void i915_forcewake_put(struct i915_device *i915, uint32_t domains);
int i915_stolen_probe(struct i915_device *i915,
                      struct i915_memory_layout *layout);
int i915_irq_install(struct i915_device *i915);
void i915_irq_uninstall(struct i915_device *i915);
void i915_debug_snapshot(struct i915_device *i915,
                         struct i915_debug_snapshot *snapshot);
void i915_probe_selftest(void);

#ifdef TEST
void i915_mock_selftest(void);
void i915_irq_test_invoke(struct i915_device *i915);
int i915_stolen_test_ownership(const uint8_t *statuses, size_t count,
                               size_t first, size_t pages, size_t *conflict);
#endif

#endif

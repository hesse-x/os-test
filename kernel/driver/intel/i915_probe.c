/*
 * Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */

#include "kernel/driver/intel/i915_probe.h"
#include "arch/x64/intel_stolen_early.h"
#include "kernel/driver/intel/i915_drv.h"
#include "kernel/driver/intel/i915_regs.h"
#include "kernel/driver/pci.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/mutex.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/spinlock.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <xos/errno.h>

#define I915_VENDOR_ID 0x8086
#define I915_DEVICE_ID 0xa780
#define I915_REVISION_ID 0x04
#define I915_BAR0_MIN_SIZE (16u * 1024u * 1024u)
#define I915_FORCEWAKE_DOMAIN_GT 1u

static const struct i915_platform_info rpls_info = {
    .device_id = I915_DEVICE_ID,
    .revision_id = I915_REVISION_ID,
    .graphics_ver = 12,
    .dma_mask_bits = 39,
    .engine_mask = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 5),
};

static bool i915_device_supported(const pci_device *pdev) {
  return pdev && pdev->vendor_id == I915_VENDOR_ID &&
         pdev->device_id == I915_DEVICE_ID &&
         pdev->revision_id == I915_REVISION_ID &&
         pdev->class_code == PCI_CLASS_DISPLAY;
}

static void i915_cleanup(struct i915_device *i915, bool failed) {
  if (!i915)
    return;
  i915->quiescing = true;
  i915->state = I915_QUIESCING;
  i915_irq_uninstall(i915);
  while (i915->forcewake_ref)
    i915_forcewake_put(i915, I915_FORCEWAKE_DOMAIN_GT);
  i915->resources &= ~I915_RES_FORCEWAKE;
  if (i915->resources & I915_RES_BAR0) {
    pci_iounmap(i915->pdev, 0);
    i915->regs = NULL;
    i915->resources &= ~I915_RES_BAR0;
  }
  if (i915->resources & I915_RES_COMMAND) {
    pci_write_config16(i915->pdev->bus, i915->pdev->dev, i915->pdev->func, 0x04,
                       i915->original_command);
    if (pci_read_config16(i915->pdev->bus, i915->pdev->dev, i915->pdev->func,
                          0x04) != i915->original_command) {
      i915->degraded = true;
      printk(LOG_ERROR,
             "i915[0000:%02x:%02x.%x]: PCI command restore mismatch\n",
             i915->pdev->bus, i915->pdev->dev, i915->pdev->func);
    }
    i915->resources &= ~I915_RES_COMMAND;
  }
  i915->state = failed ? I915_FAILED : I915_DEAD;
  pci_set_driver_private(i915->pdev, NULL);
  i915->resources &= ~I915_RES_PRIVATE;
  kfree(i915);
}

void i915_debug_snapshot(struct i915_device *i915,
                         struct i915_debug_snapshot *snapshot) {
  if (!i915 || !snapshot)
    return;
  *snapshot = (struct i915_debug_snapshot){
      .state = i915->state,
      .resources = i915->resources,
      .quiescing = i915->quiescing,
      .degraded = i915->degraded,
      .forcewake_ref = i915->forcewake_ref,
      .last_forcewake_ack = i915->last_forcewake_ack,
      .irq_count = atomic_read(&i915->irq_count),
      .irq_spurious = atomic_read(&i915->irq_spurious),
      .irq_unknown = atomic_read(&i915->irq_unknown),
  };
}

static int i915_probe_device(pci_device *pdev, const struct pci_device_id *id) {
  (void)id;
  if (!i915_device_supported(pdev))
    return -ENODEV;
  if (!pdev->bar_sizing_valid || !pdev->bar[0].size || pdev->bar[0].type == 1 ||
      pdev->bar[0].size < I915_BAR0_MIN_SIZE ||
      pdev->bar[0].phys > UINT64_MAX - pdev->bar[0].size)
    return -EINVAL;

  struct i915_device *i915 = kcalloc(1, sizeof(*i915));
  if (!i915)
    return -ENOMEM;
  i915->pdev = pdev;
  i915->info = &rpls_info;
  i915->state = I915_ALLOCATED;
  i915->resources = I915_RES_PRIVATE;
  mutex_init(&i915->lifecycle_mutex);
  mutex_init(&i915->uncore_mutex);
  i915->forcewake_lock = SPINLOCK_INIT;
  i915->irq_lock = SPINLOCK_INIT;
  atomic_set(&i915->irq_count, 0);
  atomic_set(&i915->irq_spurious, 0);
  atomic_set(&i915->irq_unknown, 0);
  pci_set_driver_private(pdev, i915);
  i915->state = I915_MATCHED;

  i915->original_command =
      pci_read_config16(pdev->bus, pdev->dev, pdev->func, 0x04);
  i915->resources |= I915_RES_COMMAND;
  if (!(i915->original_command & (1u << 1)))
    pci_write_config16(pdev->bus, pdev->dev, pdev->func, 0x04,
                       i915->original_command | (1u << 1));
  if (pci_read_config16(pdev->bus, pdev->dev, pdev->func, 0x04) !=
      (uint16_t)(i915->original_command | (1u << 1))) {
    i915_cleanup(i915, true);
    return -EIO;
  }

  i915->regs = pci_iomap(pdev, 0, false);
  if (!i915->regs) {
    i915_cleanup(i915, true);
    return -ENOMEM;
  }
  i915->regs_size = pdev->bar[0].size;
  i915->resources |= I915_RES_BAR0;
  i915->state = I915_MMIO_MAPPED;

  int rc = i915_stolen_probe(i915, &i915->memory);
  if (rc)
    goto fail;
  uint32_t ggc_a, ggc_b;
  rc = i915_mmio_read32(i915, I915_GGC, &ggc_a);
  if (!rc)
    rc = i915_mmio_read32(i915, I915_GGC, &ggc_b);
  if (!rc && (ggc_a == UINT32_MAX || ggc_a != ggc_b ||
              (uint16_t)ggc_a != i915->memory.gmch_ctrl))
    rc = -EIO;
  if (rc)
    goto fail;
  i915->state = I915_PLATFORM_PARSED;

  rc = i915_forcewake_get(i915, I915_FORCEWAKE_DOMAIN_GT);
  if (rc)
    goto fail;
  i915->resources |= I915_RES_FORCEWAKE;
  i915_forcewake_put(i915, I915_FORCEWAKE_DOMAIN_GT);
  if (i915->degraded) {
    rc = -EIO;
    goto fail;
  }
  i915->state = I915_FORCEWAKE_READY;

  rc = i915_irq_install(i915);
  if (rc)
    goto fail;
  i915->state = I915_IRQ_READY;
  i915->state = I915_PROBED;
  printk(LOG_INFO,
         "i915[0000:%02x:%02x.%x]: probe-only ready id=%04x:%04x rev=%02x "
         "subsystem=%04x:%04x cmd=%04x bar0=[0x%lx,+0x%lx] "
         "stolen=[0x%lx,+0x%lx] gms=0x%x ggms=%u ggtt=0x%lx "
         "aperture=0x%lx fw_ack=0x%x msi_vec=%d\n",
         pdev->bus, pdev->dev, pdev->func, pdev->vendor_id, pdev->device_id,
         pdev->revision_id, pdev->subsystem_vendor_id,
         pdev->subsystem_device_id, i915->original_command, pdev->bar[0].phys,
         pdev->bar[0].size, i915->memory.stolen_base, i915->memory.stolen_size,
         i915->memory.gms, i915->memory.ggms, i915->memory.ggtt_size,
         i915->memory.aperture_size, i915->last_forcewake_ack,
         pdev->msix_vector_base);
  return 0;

fail:
  printk(
      LOG_ERROR,
      "i915[0000:%02x:%02x.%x]: probe failed stage=%d rc=%d resources=0x%x\n",
      pdev->bus, pdev->dev, pdev->func, i915->state, rc, i915->resources);
  i915_cleanup(i915, true);
  return rc;
}

static void i915_probe_remove(pci_device *pdev) {
  i915_cleanup(pci_get_driver_private(pdev), false);
}

static const struct pci_device_id i915_ids[] = {
    {
        .vendor = I915_VENDOR_ID,
        .device = I915_DEVICE_ID,
        .subsystem_vendor = PCI_ANY_ID,
        .subsystem_device = PCI_ANY_ID,
        .class_id = (uint32_t)PCI_CLASS_DISPLAY << 8,
        .class_mask = 0xffff00u,
    },
    {0},
};

static const struct pci_driver i915_driver = {
    .name = "i915-probe-only",
    .id_table = i915_ids,
    .probe = i915_probe_device,
    .remove = i915_probe_remove,
};

int i915_probe_register(void) { return pci_register_driver(&i915_driver); }

void i915_probe_selftest(void) {
#ifdef TEST
  uint64_t size = 0;
  BUG_ON(!intel_gen9_stolen_size(1, &size) || size != 32u * 1024u * 1024u);
  BUG_ON(!intel_gen9_stolen_size(0xf0, &size) || size != 4u * 1024u * 1024u);
  BUG_ON(intel_gen9_stolen_size(0xff, &size));
  pci_device fake = {.vendor_id = I915_VENDOR_ID,
                     .device_id = I915_DEVICE_ID,
                     .revision_id = I915_REVISION_ID,
                     .class_code = PCI_CLASS_DISPLAY};
  BUG_ON(!i915_device_supported(&fake));
  fake.revision_id++;
  BUG_ON(i915_device_supported(&fake));
  uint32_t mock_regs[4] = {0x11223344, 0x55667788, 0, 0xa5a5a5a5};
  struct i915_device mock = {
      .regs = (void __iomem *)mock_regs,
      .regs_size = sizeof(mock_regs),
  };
  uint32_t value;
  uint64_t value64;
  BUG_ON(i915_mmio_read32(&mock, 0, &value) || value != mock_regs[0]);
  BUG_ON(i915_mmio_read32(&mock, 12, &value) || value != mock_regs[3]);
  BUG_ON(i915_mmio_read64(&mock, 0, &value64) ||
         value64 != 0x5566778811223344ULL);
  BUG_ON(i915_mmio_read32(&mock, 2, &value) != -EINVAL);
  BUG_ON(i915_mmio_read32(&mock, 16, &value) != -ERANGE);
  BUG_ON(i915_mmio_write32_allowed(&mock, 0, 0, I915_WRITE_IRQ_CONTROL) !=
         -EPERM);
  printk(LOG_INFO,
         "i915 probe-only selftest: PASS (GMS, match, MMIO bounds)\n");
  i915_mock_selftest();
#endif
}

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_DRIVER_DRIVER_H
#define KERNEL_DRIVER_DRIVER_H

#include <stddef.h>
#include <stdint.h>

struct dev_ops; // defined in kernel/bsd/devtmpfs.h

typedef struct dev_driver {
  const char *name;
  uint32_t pci_class;  // (class_subclass << 8) | prog_if, 0 = no PCI matching
  uint32_t pci_vendor; // 0 = match by class only
  uint32_t pci_device; // 0 = match by class only
  uint32_t pci_subsystem_id; // 0 = ignore subsystem id (default)
  void (*init)(void);
  struct dev_ops *ops;
} dev_driver;

void driver_register(const dev_driver *drv);
void driver_pci_match(void);
void driver_init(void);

#endif // KERNEL_DRIVER_DRIVER_H

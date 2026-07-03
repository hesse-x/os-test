/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// kernel/driver/registry.c — Driver registration and PCI auto-matching

#include "kernel/driver/driver.h"
#include "kernel/driver/pci.h"
#include "kernel/xcore/log.h"
#include <stdint.h>

#define MAX_DRIVERS 16
static dev_driver driver_table[MAX_DRIVERS];
static int driver_count;

void driver_register(const dev_driver *drv) {
  if (driver_count >= MAX_DRIVERS) {
    printk(LOG_ERROR, "driver_register: table full, cannot register %s\n",
           drv->name);
    return;
  }
  driver_table[driver_count++] = *drv;
  printk(LOG_INFO, "driver_register: %s\n", drv->name);
}

void driver_pci_match(void) {
  for (int i = 0; i < driver_count; i++) {
    dev_driver *drv = &driver_table[i];
    if (drv->pci_class == 0 && drv->pci_vendor == 0)
      continue;

    // Match by vendor/device ID if specified
    if (drv->pci_vendor != 0 && drv->pci_device != 0) {
      pci_device *dev = pci_find_device_by_id((uint16_t)drv->pci_vendor,
                                              (uint16_t)drv->pci_device);
      if (dev) {
        if (drv->pci_subsystem_id != 0) {
          /* Read subsystem id from config space (offset 0x2C bits 16-31) */
          uint32_t sub = pci_read_config(dev->bus, dev->dev, dev->func, 0x2C);
          if ((sub >> 16) != drv->pci_subsystem_id)
            continue; /* skip: subsystem mismatch */
        }
        printk(LOG_INFO,
               "driver_pci_match: %s -> PCI %02x:%02x.%02x (vendor/device)\n",
               drv->name, dev->bus, dev->dev, dev->func);
        if (drv->init)
          drv->init();
        continue;
      }
    }

    // Match by class code
    if (drv->pci_class != 0) {
      uint16_t class_subclass = (drv->pci_class >> 8) & 0xFFFF;
      uint8_t prog_if = drv->pci_class & 0xFF;

      for (int j = 0; j < pci_device_count; j++) {
        if (pci_devices[j].class_code != class_subclass)
          continue;
        // Check prog_if if specified
        if (prog_if != 0) {
          uint32_t rev_class =
              pci_read_config(pci_devices[j].bus, pci_devices[j].dev,
                              pci_devices[j].func, 0x08);
          uint8_t dev_prog_if = (rev_class >> 8) & 0xFF;
          if (dev_prog_if != prog_if)
            continue;
        }
        printk(LOG_INFO, "driver_pci_match: %s -> PCI %02x:%02x.%02x (class)\n",
               drv->name, pci_devices[j].bus, pci_devices[j].dev,
               pci_devices[j].func);
        if (drv->init)
          drv->init();
        break; // one device per driver
      }
    }
  }
}

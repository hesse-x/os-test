/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_PCI_H
#define KERNEL_PCI_H

#include "arch/x64/trap.h"
#include "kernel/xcore/sparse.h"
#include <stdbool.h>
#include <stdint.h>

// ===================== PCI constants =====================
#define MAX_PCI_DEV 64
#define PCI_MAX_BUS 256
#define PCI_MAX_DEV 32
#define PCI_MAX_FUNC 8

#define PCI_HEADER_TYPE_NORMAL 0
#define PCI_HEADER_TYPE_BRIDGE 1

// PCI class codes: (class << 8) | subclass
#define PCI_CLASS_DISPLAY 0x0300
#define PCI_CLASS_SERIAL_USB 0x0C03
#define PCI_CLASS_STORAGE 0x0100
#define PCI_CLASS_STORAGE_AHCI 0x0106
#define PCI_CLASS_NETWORK 0x0200
#define PCI_CLASS_BRIDGE_PCI 0x0604

// BAR type bits
#define PCI_BAR_IO_SPACE 0x01
#define PCI_BAR_MEM_TYPE_64 0x04

// PCI capability IDs
#define PCI_CAP_ID_MSIX 0x11
#define PCI_CAP_ID_MSI 0x05

// ===================== PCI BAR =====================
typedef struct pci_bar {
  uint64_t phys; // physical address (from BAR)
  uint64_t size; // size of the region
  void __iomem
      *vaddr;   // kernel virtual address after mapping (NULL if not mapped)
  uint8_t type; // 0=MMIO32, 1=I/O, 2=MMIO64
  int16_t map_slot;
  struct page *map_page;
  bool map_write_combining;
} pci_bar;

enum pci_irq_mode {
  PCI_IRQ_NONE = 0,
  PCI_IRQ_MSI,
  PCI_IRQ_MSIX,
};

// ===================== PCI device =====================
typedef struct pci_device {
  uint8_t bus;
  uint8_t dev;
  uint8_t func;
  uint16_t vendor_id;
  uint16_t device_id;
  uint16_t class_code; // (class << 8) | subclass
  uint32_t class_id;   // (class << 16) | (subclass << 8) | prog_if
  uint16_t subsystem_vendor_id;
  uint16_t subsystem_device_id;
  uint8_t header_type;
  uint8_t
      msi_cap_offset; // MSI capability offset in config space, 0 = not found
  uint8_t
      msix_cap_offset; // MSI-X capability offset in config space, 0 = not found
  uint8_t msix_table_bar;     // BAR index for MSI-X Table
  uint8_t msix_pba_bar;       // BAR index for PBA
  uint32_t msix_table_offset; // Offset within BAR for MSI-X Table
  uint32_t msix_pba_offset;   // Offset within BAR for PBA
  int msix_vector_base;       // First allocated vector, -1 = not allocated
  int msix_num_vectors;       // Number of vectors allocated
  enum pci_irq_mode irq_mode;
  uint32_t irq_registered_mask;
  bool enabled;
  uint64_t dma_mask;
  enum {
    PCI_BIND_UNBOUND = 0,
    PCI_BIND_PROBING,
    PCI_BIND_BOUND,
    PCI_BIND_REMOVING,
  } bind_state;
  const struct pci_driver *driver;
  void *driver_private;
  struct pci_bar bar[6];
} pci_device;

#define PCI_ANY_ID 0xffffu

struct pci_device_id {
  uint16_t vendor;
  uint16_t device;
  uint16_t subsystem_vendor;
  uint16_t subsystem_device;
  uint32_t class_id;
  uint32_t class_mask;
};

struct pci_driver {
  const char *name;
  const struct pci_device_id *id_table;
  int (*probe)(pci_device *dev, const struct pci_device_id *id);
  void (*remove)(pci_device *dev);
};

struct pci_resource_stats {
  uint32_t mapped_bars;
  uint32_t allocated_vectors;
  uint32_t registered_irqs;
  uint32_t bound_devices;
};

enum pci_fault_point {
  PCI_FAULT_NONE = 0,
  PCI_FAULT_DRIVER_REGISTER,
  PCI_FAULT_BAR_SLOT,
  PCI_FAULT_BAR_PAGE,
  PCI_FAULT_BAR_PUBLISH,
  PCI_FAULT_VECTOR_ALLOC,
  PCI_FAULT_IRQ_REGISTER,
};

// ===================== PCI global state =====================
extern pci_device pci_devices[MAX_PCI_DEV];
extern int pci_device_count;

extern void __iomem *ecam_vbase;
extern uint8_t ecam_start_bus;
extern uint8_t ecam_end_bus;

// ===================== PCI API =====================
void pci_init();

uint32_t pci_read_config(uint8_t bus, uint8_t dev, uint8_t func,
                         uint16_t offset);
void pci_write_config(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset,
                      uint32_t value);

pci_device *pci_find_device(uint16_t class_code);
pci_device *pci_find_device_by_id(uint16_t vendor, uint16_t device);

const struct pci_device_id *pci_match_id(const struct pci_device_id *ids,
                                         const pci_device *dev);
int pci_register_driver(const struct pci_driver *driver);
void pci_unregister_driver(const struct pci_driver *driver);
void pci_set_driver_private(pci_device *dev, void *private_data);
void *pci_get_driver_private(const pci_device *dev);
void pci_lifecycle_selftest(void);

int pci_enable_device(pci_device *dev);
int pci_enable_device_wc(pci_device *dev, int wc_bar_idx);
int pci_enable_device_bars(pci_device *dev, uint32_t bar_mask,
                           uint32_t wc_mask);
void pci_disable_device(pci_device *dev);
void __iomem *pci_iomap(pci_device *dev, int bar_idx, bool write_combining);
void pci_iounmap(pci_device *dev, int bar_idx);
int pci_set_dma_mask(pci_device *dev, uint8_t address_bits);
int pci_enable_msi(pci_device *dev);
int pci_request_irq(pci_device *dev, int entry, void (*handler)(trapframe *));
void pci_free_irq(pci_device *dev, int entry);
void pci_disable_interrupts(pci_device *dev);
void pci_get_resource_stats(struct pci_resource_stats *stats);
void pci_test_fail_once(enum pci_fault_point point);

// MSI-X API
int pci_enable_msix(pci_device *dev, int num_vectors);
void pci_msix_mask_entry(pci_device *dev, int entry);
void pci_msix_unmask_entry(pci_device *dev, int entry);

// sys_pci_dev_info syscall
int64_t sys_pci_dev_info(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4,
                         int64_t arg5, int64_t arg6);

#endif // KERNEL_PCI_H

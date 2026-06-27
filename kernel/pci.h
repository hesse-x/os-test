#ifndef KERNEL_PCI_H
#define KERNEL_PCI_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "kernel/sparse.h"

// ===================== PCI constants =====================
#define MAX_PCI_DEV       64
#define PCI_MAX_BUS       256
#define PCI_MAX_DEV       32
#define PCI_MAX_FUNC      8

#define PCI_HEADER_TYPE_NORMAL  0
#define PCI_HEADER_TYPE_BRIDGE  1

// PCI class codes: (class << 8) | subclass
#define PCI_CLASS_DISPLAY     0x0300
#define PCI_CLASS_SERIAL_USB  0x0C03
#define PCI_CLASS_STORAGE     0x0100
#define PCI_CLASS_STORAGE_AHCI 0x0106
#define PCI_CLASS_NETWORK     0x0200
#define PCI_CLASS_BRIDGE_PCI  0x0604

// BAR type bits
#define PCI_BAR_IO_SPACE    0x01
#define PCI_BAR_MEM_TYPE_64 0x04

// PCI capability IDs
#define PCI_CAP_ID_MSIX     0x11
#define PCI_CAP_ID_MSI      0x05

// ===================== PCI BAR =====================
typedef struct pci_bar {
  uint64_t phys;       // physical address (from BAR)
  uint64_t size;       // size of the region
  void __iomem *vaddr; // kernel virtual address after mapping (NULL if not mapped)
  uint8_t  type;       // 0=MMIO32, 1=I/O, 2=MMIO64
} pci_bar_t;

// ===================== PCI device =====================
typedef struct pci_device {
  uint8_t  bus;
  uint8_t  dev;
  uint8_t  func;
  uint16_t vendor_id;
  uint16_t device_id;
  uint16_t class_code;    // (class << 8) | subclass
  uint8_t  header_type;
  uint8_t  msi_cap_offset;   // MSI capability offset in config space, 0 = not found
  uint8_t  msix_cap_offset;  // MSI-X capability offset in config space, 0 = not found
  uint8_t  msix_table_bar;   // BAR index for MSI-X Table
  uint8_t  msix_pba_bar;     // BAR index for PBA
  uint32_t msix_table_offset; // Offset within BAR for MSI-X Table
  uint32_t msix_pba_offset;   // Offset within BAR for PBA
  int      msix_vector_base;  // First allocated vector, -1 = not allocated
  int      msix_num_vectors;  // Number of vectors allocated
  bool     enabled;
  struct pci_bar bar[6];
} pci_device_t;

// ===================== PCI global state =====================
extern pci_device_t pci_devices[MAX_PCI_DEV];
extern int pci_device_count;

extern void __iomem *ecam_vbase;
extern uint8_t  ecam_start_bus;
extern uint8_t  ecam_end_bus;

// ===================== PCI API =====================
void pci_init();

uint32_t pci_read_config(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset);
void     pci_write_config(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset, uint32_t value);

pci_device_t *pci_find_device(uint16_t class_code);
pci_device_t *pci_find_device_by_id(uint16_t vendor, uint16_t device);

int pci_enable_device(pci_device_t *dev);
int pci_enable_device_wc(pci_device_t *dev, int wc_bar_idx);
int pci_enable_msi(pci_device_t *dev);

// MSI-X API
int pci_enable_msix(pci_device_t *dev, int num_vectors);
void pci_msix_mask_entry(pci_device_t *dev, int entry);
void pci_msix_unmask_entry(pci_device_t *dev, int entry);
void __iomem *pci_msix_table_addr(pci_device_t *dev);
void __iomem *pci_msix_pba_addr(pci_device_t *dev);
int pci_msix_vector_base(pci_device_t *dev);

// sys_pci_dev_info is declared in kernel/trap.h

#endif // KERNEL_PCI_H

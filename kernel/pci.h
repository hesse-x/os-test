#ifndef KERNEL_PCI_H
#define KERNEL_PCI_H

#include <stdint.h>
#include <stddef.h>

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
#define PCI_CLASS_NETWORK     0x0200
#define PCI_CLASS_BRIDGE_PCI  0x0604

// BAR type bits
#define PCI_BAR_IO_SPACE    0x01
#define PCI_BAR_MEM_TYPE_64 0x04

// ===================== PCI BAR =====================
struct pci_bar {
  uint64_t phys;       // physical address (from BAR)
  uint64_t size;       // size of the region
  uint64_t vaddr;      // kernel virtual address after mapping (0 if not mapped)
  uint8_t  type;       // 0=MMIO32, 1=I/O, 2=MMIO64
};

// ===================== PCI device =====================
struct pci_device {
  uint8_t  bus;
  uint8_t  dev;
  uint8_t  func;
  uint16_t vendor_id;
  uint16_t device_id;
  uint16_t class_code;    // (class << 8) | subclass
  uint8_t  header_type;
  uint8_t  irq_pin;
  uint8_t  irq_line;
  bool     enabled;
  struct pci_bar bar[6];
};

// ===================== PCI global state =====================
extern struct pci_device pci_devices[MAX_PCI_DEV];
extern int pci_device_count;

extern uint64_t ecam_vbase;
extern uint8_t  ecam_start_bus;
extern uint8_t  ecam_end_bus;

// ===================== PCI API =====================
void pci_init();

uint32_t pci_read_config(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
void     pci_write_config(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t value);

struct pci_device *pci_find_device(uint16_t class_code);
struct pci_device *pci_find_device_by_id(uint16_t vendor, uint16_t device);

int pci_enable_device(struct pci_device *dev);

// sys_pci_dev_info is declared in kernel/trap.h (extern "C")

#endif // KERNEL_PCI_H

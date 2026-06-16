#include "kernel/pci.h"
#include "kernel/acpi.h"
#include "arch/x64/paging.h"
#include "kernel/serial.h"
#include "kernel/mem/alloc.h"
#include "common/macro.h"
#include "common/errno.h"

struct pci_device pci_devices[MAX_PCI_DEV];
int pci_device_count = 0;

uint64_t ecam_vbase = 0;
uint8_t  ecam_start_bus = 0;
uint8_t  ecam_end_bus = 0;

// ===================== ECAM MMIO mapping =====================

static void map_ecam_mmio(uint64_t ecam_phys, uint8_t start_bus, uint8_t end_bus) {
  uint64_t region_start = ecam_phys + (uint64_t)start_bus * 0x100000;
  uint64_t region_end = ecam_phys + ((uint64_t)end_bus + 1) * 0x100000;
  region_start &= ~0x1FFFFFULL;   // 2MB align down
  region_end = (region_end + 0x1FFFFFULL) & ~0x1FFFFFULL; // 2MB align up
  size_t num_2mb = (region_end - region_start) / 0x200000;

  // Find free PDPT_hh slot
  int pdpt_idx = -1;
  for (int i = 511; i >= 0; i--) {
    if (pdpt_hh[i] == 0) {
      pdpt_idx = i;
      break;
    }
  }
  if (pdpt_idx < 0) {
    serial_puts("pci: no free PDPT_hh slot for ECAM\n");
    halt();
  }

  // Allocate PD using bfc_alloc (bump is disabled at pci_init time)
  Page *pd_page = bfc_alloc.alloc_page(1);
  if (!pd_page) {
    serial_puts("pci: ECAM PD alloc failed\n");
    halt();
  }
  uint64_t *pd = (uint64_t *)phys_to_virt(page_to_phys(pd_page));
  for (int i = 0; i < 512; i++) pd[i] = 0;

  // Fill PD with 2MB huge pages, PCD+PWT for uncacheable MMIO
  // 0x9B = Present + RW + PS + PCD + PWT
  for (size_t n = 0; n < num_2mb; n++) {
    pd[n] = (region_start + n * 0x200000) | 0x9B;
  }

  pdpt_hh[pdpt_idx] = page_to_phys(pd_page) | PTE_PRESENT | PTE_RW;

  // Compute ecam_vbase so ecam_vbase + bus<<20 + dev<<15 + func<<12 + offset
  // addresses the config register
  uint64_t vma = VMA_BASE + (uint64_t)(pdpt_idx - 510) * 0x40000000;
  ecam_vbase = vma + (ecam_phys - region_start);

  device_vma_base = vma + num_2mb * 0x200000;
  flush_tlb();

  serial_puts("pci: ECAM mapped vbase=");
  serial_put_hex(ecam_vbase);
  serial_puts("\n");
}

// ===================== Config space access =====================

uint32_t pci_read_config(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset) {
  uint64_t addr = ecam_vbase
                + ((uint64_t)bus << 20)
                + ((uint64_t)dev << 15)
                + ((uint64_t)func << 12)
                + offset;
  return *(volatile uint32_t *)addr;
}

void pci_write_config(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset, uint32_t value) {
  uint64_t addr = ecam_vbase
                + ((uint64_t)bus << 20)
                + ((uint64_t)dev << 15)
                + ((uint64_t)func << 12)
                + offset;
  *(volatile uint32_t *)addr = value;
}

// ===================== BAR sizing =====================

static void pci_size_bar(struct pci_device *d, int bar_idx) {
  uint8_t bus = d->bus, dev = d->dev, func = d->func;
  uint8_t offset = 0x10 + bar_idx * 4;

  uint32_t orig = pci_read_config(bus, dev, func, offset);
  if (orig == 0 || orig == 0xFFFFFFFF) {
    d->bar[bar_idx].phys = 0;
    d->bar[bar_idx].size = 0;
    d->bar[bar_idx].type = 0;
    return;
  }

  bool is_io = (orig & PCI_BAR_IO_SPACE);
  bool is_64 = (!is_io && (orig & PCI_BAR_MEM_TYPE_64));

  // Write all 1s, read back mask, restore original
  pci_write_config(bus, dev, func, offset, 0xFFFFFFFF);
  uint32_t mask = pci_read_config(bus, dev, func, offset);
  pci_write_config(bus, dev, func, offset, orig);

  if (is_io) {
    uint16_t io_size = ~(uint16_t)(mask & ~0x3) + 1;
    d->bar[bar_idx].phys = orig & ~0x3;
    d->bar[bar_idx].size = io_size;
    d->bar[bar_idx].type = 1; // I/O
  } else if (is_64) {
    // Also read/write the high 32 bits
    uint32_t orig_hi = pci_read_config(bus, dev, func, offset + 4);
    pci_write_config(bus, dev, func, offset + 4, 0xFFFFFFFF);
    uint32_t mask_hi = pci_read_config(bus, dev, func, offset + 4);
    pci_write_config(bus, dev, func, offset + 4, orig_hi);

    // Restore low first (already done above, but ensure)
    pci_write_config(bus, dev, func, offset, orig);
    pci_write_config(bus, dev, func, offset + 4, orig_hi);

    uint64_t size64 = ((uint64_t)mask_hi << 32) | (mask & ~0xFU);
    size64 = ~size64 + 1;
    d->bar[bar_idx].phys = ((uint64_t)orig_hi << 32) | (orig & ~0xFU);
    d->bar[bar_idx].size = size64;
    d->bar[bar_idx].type = 2; // MMIO64
    // Mark next BAR as consumed
    d->bar[bar_idx + 1].phys = 0;
    d->bar[bar_idx + 1].size = 0;
    d->bar[bar_idx + 1].type = 0;
  } else {
    uint32_t size32 = ~(mask & ~0xFU) + 1;
    d->bar[bar_idx].phys = orig & ~0xFU;
    d->bar[bar_idx].size = size32;
    d->bar[bar_idx].type = 0; // MMIO32
  }
}

// ===================== Device scanning =====================

static void pci_scan_bus(uint8_t bus);

static void pci_scan_function(uint8_t bus, uint8_t dev, uint8_t func) {
  if (pci_device_count >= MAX_PCI_DEV) return;

  uint32_t vd = pci_read_config(bus, dev, func, 0x00);
  uint16_t vendor = vd & 0xFFFF;
  if (vendor == 0xFFFF) return;

  uint16_t device = (vd >> 16) & 0xFFFF;
  uint32_t rev_class = pci_read_config(bus, dev, func, 0x08);
  uint8_t header_type = (pci_read_config(bus, dev, func, 0x0C) >> 16) & 0xFF;
  uint16_t class_code = (rev_class >> 16) & 0xFFFF;

  uint32_t irq_info = pci_read_config(bus, dev, func, 0x3C);
  uint8_t irq_pin = (irq_info >> 8) & 0xFF;
  uint8_t irq_line = irq_info & 0xFF;

  struct pci_device *d = &pci_devices[pci_device_count];
  d->bus = bus;
  d->dev = dev;
  d->func = func;
  d->vendor_id = vendor;
  d->device_id = device;
  d->class_code = class_code;
  d->header_type = header_type & 0x7F;
  d->irq_pin = irq_pin;
  d->irq_line = irq_line;
  d->enabled = false;

  // Size all BARs
  int max_bars = (d->header_type == PCI_HEADER_TYPE_BRIDGE) ? 2 : 6;
  for (int i = 0; i < max_bars; i++) {
    pci_size_bar(d, i);
    if (d->bar[i].type == 2 && i < max_bars - 1) i++; // skip next (consumed by 64-bit)
  }

  pci_device_count++;

  serial_puts("pci: ");
  serial_put_hex(bus);
  serial_puts(":");
  serial_put_hex(dev);
  serial_puts(".");
  serial_put_hex(func);
  serial_puts(" vendor=");
  serial_put_hex(vendor);
  serial_puts(" device=");
  serial_put_hex(device);
  serial_puts(" class=");
  serial_put_hex(class_code);
  if ((header_type & 0x7F) == PCI_HEADER_TYPE_BRIDGE)
    serial_puts(" [BRIDGE]");
  serial_puts("\n");

  // If PCI-to-PCI bridge, scan secondary bus
  if ((header_type & 0x7F) == PCI_HEADER_TYPE_BRIDGE) {
    uint32_t bus_info = pci_read_config(bus, dev, func, 0x18);
    uint8_t secondary = (bus_info >> 8) & 0xFF;
    if (secondary != bus && secondary < PCI_MAX_BUS) {
      pci_scan_bus(secondary);
    }
  }
}

static void pci_scan_bus(uint8_t bus) {
  for (int dev = 0; dev < PCI_MAX_DEV; dev++) {
    uint32_t vd = pci_read_config(bus, dev, 0, 0x00);
    if ((vd & 0xFFFF) == 0xFFFF) continue;

    uint8_t header_type = (pci_read_config(bus, dev, 0, 0x0C) >> 16) & 0xFF;

    if (header_type & 0x80) {
      // Multi-function device
      for (int func = 0; func < PCI_MAX_FUNC; func++) {
        pci_scan_function(bus, dev, func);
      }
    } else {
      pci_scan_function(bus, dev, 0);
    }
  }
}

// ===================== BAR MMIO mapping =====================

static void pci_map_bar_mmio(struct pci_device *d) {
  int max_bars = (d->header_type == PCI_HEADER_TYPE_BRIDGE) ? 2 : 6;
  for (int i = 0; i < max_bars; i++) {
    if (d->bar[i].size == 0) continue;
    if (d->bar[i].type == 1) continue; // I/O BAR, no mapping needed

    uint64_t phys = d->bar[i].phys;
    uint64_t size = d->bar[i].size;
    uint64_t region_start = phys & ~0x1FFFFFULL;
    uint64_t region_end = ALIGN_UP(phys + size, 0x200000);
    size_t num_2mb = (region_end - region_start) / 0x200000;

    // Find free PDPT_hh slot
    int pdpt_idx = -1;
    for (int j = 511; j >= 0; j--) {
      if (pdpt_hh[j] == 0) { pdpt_idx = j; break; }
    }
    if (pdpt_idx < 0) {
      serial_puts("pci: no free PDPT_hh slot for BAR\n");
      continue;
    }

    // Allocate PD
    Page *pd_page = bfc_alloc.alloc_page(1);
    if (!pd_page) continue;
    uint64_t *pd = (uint64_t *)phys_to_virt(page_to_phys(pd_page));
    for (int j = 0; j < 512; j++) pd[j] = 0;

    // Fill with 2MB huge pages, PCD+PWT for MMIO
    for (size_t n = 0; n < num_2mb; n++) {
      pd[n] = (region_start + n * 0x200000) | 0x9B;
    }

    pdpt_hh[pdpt_idx] = page_to_phys(pd_page) | PTE_PRESENT | PTE_RW;

    uint64_t vma = VMA_BASE + (uint64_t)(pdpt_idx - 510) * 0x40000000;
    d->bar[i].vaddr = vma + (phys - region_start);
    device_vma_base = vma + num_2mb * 0x200000;
    flush_tlb();
  }
}

// ===================== Device enablement =====================

int pci_enable_device(struct pci_device *d) {
  if (d->enabled) return 0;

  // 1. Map MMIO BARs
  pci_map_bar_mmio(d);

  // 2. Enable Bus Master + Memory Space
  uint32_t cmd = pci_read_config(d->bus, d->dev, d->func, 0x04);
  cmd |= (1 << 1) | (1 << 2);  // Bus Master + Memory Space
  pci_write_config(d->bus, d->dev, d->func, 0x04, cmd);

  d->enabled = true;
  return 0;
}

// ===================== Device lookup =====================

struct pci_device *pci_find_device(uint16_t class_code) {
  for (int i = 0; i < pci_device_count; i++) {
    if (pci_devices[i].class_code == class_code)
      return &pci_devices[i];
  }
  return nullptr;
}

struct pci_device *pci_find_device_by_id(uint16_t vendor, uint16_t device) {
  for (int i = 0; i < pci_device_count; i++) {
    if (pci_devices[i].vendor_id == vendor && pci_devices[i].device_id == device)
      return &pci_devices[i];
  }
  return nullptr;
}

// ===================== Syscall: sys_pci_dev_info =====================

extern "C"
uint64_t sys_pci_dev_info(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t) {
  uint8_t bus = (uint8_t)arg1;
  uint8_t dev_num = (uint8_t)arg2;
  uint8_t func = (uint8_t)arg3;
  void *out_ptr = (void *)arg4;

  // Validate user pointer
  uint64_t ptr = (uint64_t)out_ptr;
  if (!ptr || ptr >= 0xFFFFFFFF80000000ULL ||
      ptr + sizeof(struct pci_dev_info) > 0xFFFFFFFF80000000ULL)
    return (uint64_t)EFAULT;

  // Find the device
  struct pci_device *d = nullptr;
  for (int i = 0; i < pci_device_count; i++) {
    if (pci_devices[i].bus == bus && pci_devices[i].dev == dev_num &&
        pci_devices[i].func == func) {
      d = &pci_devices[i];
      break;
    }
  }
  if (!d) return (uint64_t)ENOENT;

  struct pci_dev_info info = {};
  info.vendor_id = d->vendor_id;
  info.device_id = d->device_id;
  info.class_code = d->class_code;
  info.irq_pin = d->irq_pin;
  info.irq_line = d->irq_line;
  info.num_bars = (d->header_type == PCI_HEADER_TYPE_BRIDGE) ? 2 : 6;
  for (int i = 0; i < info.num_bars; i++) {
    info.bars[i].phys = d->bar[i].phys;
    info.bars[i].size = d->bar[i].size;
    info.bars[i].type = d->bar[i].type;
  }

  __memcpy(out_ptr, &info, sizeof(info));
  return 0;
}

// ===================== pci_init =====================

void pci_init() {
  serial_puts("pci_init\n");

  if (g_mcfg.ecam_base == 0) {
    serial_puts("pci: no MCFG/ECAM, skipping\n");
    return;
  }

  ecam_start_bus = g_mcfg.start_bus;
  ecam_end_bus = g_mcfg.end_bus;

  // 1. Map ECAM configuration space
  map_ecam_mmio(g_mcfg.ecam_base, ecam_start_bus, ecam_end_bus);

  // 2. Scan buses
  pci_scan_bus(ecam_start_bus);

  serial_puts("pci: found ");
  serial_put_hex(pci_device_count);
  serial_puts(" devices\n");
}

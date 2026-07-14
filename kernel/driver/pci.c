/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/driver/pci.h"
#include "arch/x64/apic.h"
#include "arch/x64/paging.h"
#include "arch/x64/utils.h"
#include "kernel/xcore/acpi.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/mem/kasan.h"
#include "utils/macro.h"
#include <stdbool.h>
#include <stddef.h>
#include <xos/errno.h>
#include <xos/syscall_nums.h>

pci_device pci_devices[MAX_PCI_DEV];
int pci_device_count = 0;

void __iomem *ecam_vbase = NULL;
uint8_t ecam_start_bus = 0;
uint8_t ecam_end_bus = 0;

// MSI-X vector allocation: simple counter starting at 64
static int next_msix_vector = 64;

// ===================== ECAM MMIO mapping =====================

__attribute__((no_sanitize("kernel-address"))) static void
map_ecam_mmio(uint64_t ecam_phys, uint8_t start_bus, uint8_t end_bus) {
  uint64_t region_start = ecam_phys + (uint64_t)start_bus * 0x100000;
  uint64_t region_end = ecam_phys + ((uint64_t)end_bus + 1) * 0x100000;
  region_start &= ~0x1FFFFFULL;                           // 2MB align down
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
    printk(LOG_ERROR, "pci: no free PDPT_hh slot for ECAM\n");
    halt();
  }

  // Allocate PD using bfc_alloc (bump is disabled at pci_init time)
  struct page *pd_page = bfc_alloc_page(1);
  if (!pd_page) {
    printk(LOG_ERROR, "pci: ECAM PD alloc failed\n");
    halt();
  }
  uint64_t *pd = (__force uint64_t *)phys_to_virt(
      (__force phys_addr_t)page_to_phys(pd_page));
  for (int i = 0; i < 512; i++)
    pd[i] = 0;

  // Fill PD with 2MB huge pages, UC for MMIO
  for (size_t n = 0; n < num_2mb; n++) {
    pd[n] =
        (region_start + n * 0x200000) | PTE_PRESENT | PTE_RW | PTE_PS | PTE_UC;
  }

  pdpt_hh[pdpt_idx] =
      (__force uint64_t)page_to_phys(pd_page) | PTE_PRESENT | PTE_RW;

  // Compute ecam_vbase so ecam_vbase + bus<<20 + dev<<15 + func<<12 + offset
  // addresses the config register
  uint64_t vma =
      (0xFFFFULL << 48) | (511ULL << 39) | ((uint64_t)pdpt_idx << 30);
  ecam_vbase = (void __iomem __force *)(vma + (ecam_phys - region_start));

  device_vma_base = vma + num_2mb * 0x200000;
  flush_tlb();
}

// ===================== Config space access =====================

__attribute__((no_sanitize("kernel-address"))) uint32_t
pci_read_config(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset) {
  volatile uint32_t __iomem *addr =
      (volatile uint32_t __iomem *)ecam_vbase +
      (((uint64_t)bus << 20) + ((uint64_t)dev << 15) + ((uint64_t)func << 12) +
       offset) /
          4;
  return *(volatile uint32_t __force *)addr;
}

__attribute__((no_sanitize("kernel-address"))) void
pci_write_config(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset,
                 uint32_t value) {
  volatile uint32_t __iomem *addr =
      (volatile uint32_t __iomem *)ecam_vbase +
      (((uint64_t)bus << 20) + ((uint64_t)dev << 15) + ((uint64_t)func << 12) +
       offset) /
          4;
  *(volatile uint32_t __force *)addr = value;
}

// ===================== BAR sizing =====================

static void pci_size_bar(pci_device *d, int bar_idx) {
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

  // Write all 1s, read back mask (hardwired bits survive, decode bits read as
  // 0)
  pci_write_config(bus, dev, func, offset, 0xFFFFFFFF);
  uint32_t mask = pci_read_config(bus, dev, func, offset);

  // Determine 64-bit from mask's type bits (more reliable than orig)
  // PCI spec: hardwired bits [3:0] for MMIO always survive the all-1s write
  bool is_64 = (!is_io && (mask & PCI_BAR_MEM_TYPE_64));

  if (is_io) {
    pci_write_config(bus, dev, func, offset, orig);
    uint16_t io_size = ~(uint16_t)(mask & ~0x3) + 1;
    d->bar[bar_idx].phys = orig & ~0x3;
    d->bar[bar_idx].size = io_size;
    d->bar[bar_idx].type = 1; // I/O
  } else if (is_64) {
    // Also size the high 32 bits (next BAR) before restoring low
    uint32_t orig_hi = pci_read_config(bus, dev, func, offset + 4);
    pci_write_config(bus, dev, func, offset + 4, 0xFFFFFFFF);
    uint32_t mask_hi = pci_read_config(bus, dev, func, offset + 4);
    // Restore both halves
    pci_write_config(bus, dev, func, offset + 4, orig_hi);
    pci_write_config(bus, dev, func, offset, orig);

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
    pci_write_config(bus, dev, func, offset, orig);
    uint32_t size32 = ~(mask & ~0xFU) + 1;
    d->bar[bar_idx].phys = orig & ~0xFU;
    d->bar[bar_idx].size = size32;
    d->bar[bar_idx].type = 0; // MMIO32
  }
}

// ===================== Device scanning =====================

static void pci_scan_bus(uint8_t bus);

__attribute__((no_sanitize("kernel-address"))) static void
pci_scan_function(uint8_t bus, uint8_t dev, uint8_t func) {
  if (pci_device_count >= MAX_PCI_DEV)
    return;

  uint32_t vd = pci_read_config(bus, dev, func, 0x00);
  uint16_t vendor = vd & 0xFFFF;
  if (vendor == 0xFFFF)
    return;

  uint16_t device = (vd >> 16) & 0xFFFF;
  uint32_t rev_class = pci_read_config(bus, dev, func, 0x08);
  uint8_t header_type = (pci_read_config(bus, dev, func, 0x0C) >> 16) & 0xFF;
  uint16_t class_code = (rev_class >> 16) & 0xFFFF;

  pci_device *d = &pci_devices[pci_device_count];
  d->bus = bus;
  d->dev = dev;
  d->func = func;
  d->vendor_id = vendor;
  d->device_id = device;
  d->class_code = class_code;
  d->header_type = header_type & 0x7F;
  d->msix_cap_offset = 0;
  d->msi_cap_offset = 0;
  d->msix_table_bar = 0;
  d->msix_pba_bar = 0;
  d->msix_table_offset = 0;
  d->msix_pba_offset = 0;
  d->msix_vector_base = -1;
  d->msix_num_vectors = 0;
  d->enabled = false;

  // Walk PCI capability chain (Type 0 header only)
  if (d->header_type == PCI_HEADER_TYPE_NORMAL) {
    uint8_t cap_ptr = (pci_read_config(bus, dev, func, 0x34) & 0xFC);
    while (cap_ptr != 0) {
      uint32_t cap_word = pci_read_config(bus, dev, func, cap_ptr);
      uint8_t cap_id = cap_word & 0xFF;
      uint8_t next_ptr = (cap_word >> 8) & 0xFC;
      if (cap_id == PCI_CAP_ID_MSIX) {
        d->msix_cap_offset = cap_ptr;
        uint32_t table_info = pci_read_config(bus, dev, func, cap_ptr + 4);
        uint32_t pba_info = pci_read_config(bus, dev, func, cap_ptr + 8);
        d->msix_table_bar = table_info & 0x7;
        d->msix_table_offset = table_info & ~0x7;
        d->msix_pba_bar = pba_info & 0x7;
        d->msix_pba_offset = pba_info & ~0x7;
      } else if (cap_id == PCI_CAP_ID_MSI) {
        d->msi_cap_offset = cap_ptr;
      }
      cap_ptr = next_ptr;
      if (cap_ptr < 0x40)
        break; // invalid, stop
    }
  }

  // Size all BARs
  int max_bars = (d->header_type == PCI_HEADER_TYPE_BRIDGE) ? 2 : 6;
  for (int i = 0; i < max_bars; i++) {
    pci_size_bar(d, i);
    if (d->bar[i].type == 2 && i < max_bars - 1)
      i++; // skip next (consumed by 64-bit)
  }

  pci_device_count++;

  // If PCI-to-PCI bridge, scan secondary bus
  if ((header_type & 0x7F) == PCI_HEADER_TYPE_BRIDGE) {
    uint32_t bus_info = pci_read_config(bus, dev, func, 0x18);
    uint8_t secondary = (bus_info >> 8) & 0xFF;
    if (secondary != bus) {
      pci_scan_bus(secondary);
    }
  }
}

__attribute__((no_sanitize("kernel-address"))) static void
pci_scan_bus(uint8_t bus) {
  for (int dev = 0; dev < PCI_MAX_DEV; dev++) {
    uint32_t vd = pci_read_config(bus, dev, 0, 0x00);
    if ((vd & 0xFFFF) == 0xFFFF)
      continue;

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

__attribute__((no_sanitize("kernel-address"))) static void
pci_map_bar_mmio(pci_device *d, int wc_bar_idx) {
  int max_bars = (d->header_type == PCI_HEADER_TYPE_BRIDGE) ? 2 : 6;
  for (int i = 0; i < max_bars; i++) {
    if (d->bar[i].size == 0)
      continue;
    if (d->bar[i].type == 1)
      continue; // I/O BAR, no mapping needed

    uint64_t phys = d->bar[i].phys;
    uint64_t size = d->bar[i].size;

    // Find free PDPT_hh slot
    int pdpt_idx = -1;
    for (int j = 511; j >= 0; j--) {
      if (pdpt_hh[j] == 0) {
        pdpt_idx = j;
        break;
      }
    }
    if (pdpt_idx < 0) {
      printk(LOG_ERROR, "pci: no free PDPT_hh slot for BAR\n");
      continue;
    }

    uint64_t region_start = phys & ~0x1FFFFFULL;
    uint64_t region_end = ALIGN_UP(phys + size, 0x200000);
    size_t num_2mb = (region_end - region_start) / 0x200000;

    // Allocate PD
    struct page *pd_page = bfc_alloc_page(1);
    if (!pd_page)
      continue;
    uint64_t *pd = (__force uint64_t *)phys_to_virt(
        (__force phys_addr_t)page_to_phys(pd_page));
    for (int j = 0; j < 512; j++)
      pd[j] = 0;

    // Fill with 2MB huge pages: WC for specified BAR, UC for others
    uint64_t cache_flags = (i == wc_bar_idx) ? PTE_WC : PTE_UC;
    for (size_t n = 0; n < num_2mb; n++) {
      pd[n] = (region_start + n * 0x200000) | PTE_PRESENT | PTE_RW | PTE_PS |
              cache_flags;
    }

    pdpt_hh[pdpt_idx] =
        (__force uint64_t)page_to_phys(pd_page) | PTE_PRESENT | PTE_RW;

    uint64_t vma =
        (0xFFFFULL << 48) | (511ULL << 39) | ((uint64_t)pdpt_idx << 30);
    d->bar[i].vaddr = (void __iomem __force *)(vma + (phys - region_start));
    device_vma_base = vma + num_2mb * 0x200000;
    flush_tlb();
  }
}

// ===================== Device enablement =====================

__attribute__((no_sanitize("kernel-address"))) int
pci_enable_device(pci_device *d) {
  if (d->enabled)
    return 0;

  // 1. Map MMIO BARs
  pci_map_bar_mmio(d, -1); // all BARs UC

  // 2. Enable Bus Master + Memory Space
  uint32_t cmd = pci_read_config(d->bus, d->dev, d->func, 0x04);
  cmd |= (1 << 1) | (1 << 2); // Bus Master + Memory Space
  pci_write_config(d->bus, d->dev, d->func, 0x04, cmd);

  d->enabled = true;
  return 0;
}

__attribute__((no_sanitize("kernel-address"))) int
pci_enable_device_wc(pci_device *d, int wc_bar_idx) {
  if (d->enabled)
    return 0;

  // 1. Map MMIO BARs (specified BAR uses WC, others UC)
  pci_map_bar_mmio(d, wc_bar_idx);

  // 2. Enable Bus Master + Memory Space
  uint32_t cmd = pci_read_config(d->bus, d->dev, d->func, 0x04);
  cmd |= (1 << 1) | (1 << 2); // Bus Master + Memory Space
  pci_write_config(d->bus, d->dev, d->func, 0x04, cmd);

  d->enabled = true;
  return 0;
}

// ===================== Device lookup =====================

__attribute__((no_sanitize("kernel-address"))) pci_device *
pci_find_device(uint16_t class_code) {
  for (int i = 0; i < pci_device_count; i++) {
    if (pci_devices[i].class_code == class_code)
      return &pci_devices[i];
  }
  return NULL;
}

__attribute__((no_sanitize("kernel-address"))) pci_device *
pci_find_device_by_id(uint16_t vendor, uint16_t device) {
  for (int i = 0; i < pci_device_count; i++) {
    if (pci_devices[i].vendor_id == vendor &&
        pci_devices[i].device_id == device)
      return &pci_devices[i];
  }
  return NULL;
}

// ===================== Syscall: sys_pci_dev_info =====================

int64_t sys_pci_dev_info(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4,
                         int64_t unused5, int64_t unused6) {
  uint8_t bus = (uint8_t)arg1;
  uint8_t dev_num = (uint8_t)arg2;
  uint8_t func = (uint8_t)arg3;
  void __user *out_ptr = (void __user *)arg4;

  // Validate user pointer
  uint64_t ptr = (uint64_t)out_ptr;
  if (!ptr || ptr >= 0xFFFFFFFF80000000ULL ||
      ptr + sizeof(struct pci_dev_info) > 0xFFFFFFFF80000000ULL)
    return (int64_t)-EFAULT;

  // Find the device
  pci_device *d = NULL;
  for (int i = 0; i < pci_device_count; i++) {
    if (pci_devices[i].bus == bus && pci_devices[i].dev == dev_num &&
        pci_devices[i].func == func) {
      d = &pci_devices[i];
      break;
    }
  }
  if (!d)
    return (int64_t)-ENOENT;

  struct pci_dev_info info = {0};
  info.vendor_id = d->vendor_id;
  info.device_id = d->device_id;
  info.class_code = d->class_code;
  info.num_bars = (d->header_type == PCI_HEADER_TYPE_BRIDGE) ? 2 : 6;
  for (int i = 0; i < info.num_bars; i++) {
    info.bars[i].phys = d->bar[i].phys;
    info.bars[i].size = d->bar[i].size;
    info.bars[i].type = d->bar[i].type;
  }

  if (copy_to_user(out_ptr, &info, sizeof(info)))
    return (int64_t)-EFAULT;
  return 0;
}

// ===================== MSI =====================

__attribute__((no_sanitize("kernel-address"))) int
pci_enable_msi(pci_device *dev) {
  if (dev->msi_cap_offset == 0)
    return -ENOSYS;
  if (next_msix_vector > 95)
    return -ENOMEM; // cap at vec 95 (priority class 5)

  // Read Message Control (upper 16 bits of DWORD at cap_offset)
  uint32_t cap_dword =
      pci_read_config(dev->bus, dev->dev, dev->func, dev->msi_cap_offset);
  uint16_t msg_ctrl = (cap_dword >> 16) & 0xFFFF;
  bool is_64bit = (msg_ctrl & (1 << 7)) != 0; // 64-bit Address Capable

  // Allocate one vector
  int vector = next_msix_vector++;
  dev->msix_vector_base = vector; // reuse field for MSI vector base
  dev->msix_num_vectors = 1;

  uint32_t bsp_apic_id = (uint32_t)(lapic_read(LAPIC_ID) >> 24);

  // Write Message Address (DWORD at cap_offset + 4)
  pci_write_config(dev->bus, dev->dev, dev->func, dev->msi_cap_offset + 4,
                   0xFEE00000 | (bsp_apic_id << 12));

  if (is_64bit) {
    // Write Message Upper Address (DWORD at cap_offset + 8) = 0
    pci_write_config(dev->bus, dev->dev, dev->func, dev->msi_cap_offset + 8, 0);
    // Write Message Data (DWORD at cap_offset + 12)
    pci_write_config(dev->bus, dev->dev, dev->func, dev->msi_cap_offset + 12,
                     vector);
  } else {
    // Write Message Data (DWORD at cap_offset + 8)
    pci_write_config(dev->bus, dev->dev, dev->func, dev->msi_cap_offset + 8,
                     vector);
  }

  // Enable MSI: set Enable bit (bit 0 of 16-bit Message Control)
  cap_dword =
      pci_read_config(dev->bus, dev->dev, dev->func, dev->msi_cap_offset);
  msg_ctrl = (cap_dword >> 16) & 0xFFFF;
  msg_ctrl |= (1 << 0); // MSI Enable
  cap_dword = (cap_dword & 0xFFFF) | ((uint32_t)msg_ctrl << 16);
  pci_write_config(dev->bus, dev->dev, dev->func, dev->msi_cap_offset,
                   cap_dword);

  // Disable INTx: set bit 10 (Interrupt Disable) in Command register
  uint32_t cmd = pci_read_config(dev->bus, dev->dev, dev->func, 0x04);
  cmd |= (1 << 10);
  pci_write_config(dev->bus, dev->dev, dev->func, 0x04, cmd);

  return 0;
}

// ===================== MSI-X =====================

__attribute__((no_sanitize("kernel-address"))) int
pci_enable_msix(pci_device *dev, int num_vectors) {
  if (dev->msix_cap_offset == 0)
    return -ENOSYS;
  if (num_vectors <= 0 || next_msix_vector + num_vectors > 96)
    return -ENOMEM;

  // Read Message Control to get table size (bits 10:2 of 16-bit Message Control
  // = N-1) Message Control is at cap_offset+2, upper 16 bits of DWORD at
  // cap_offset
  uint32_t cap_dword =
      pci_read_config(dev->bus, dev->dev, dev->func, dev->msix_cap_offset);
  uint16_t msg_ctrl = (cap_dword >> 16) & 0xFFFF;
  int table_size = ((msg_ctrl >> 2) & 0x7FF) + 1;
  if (num_vectors > table_size)
    num_vectors = table_size;

  // Allocate vectors
  int base = next_msix_vector;
  next_msix_vector += num_vectors;
  dev->msix_vector_base = base;
  dev->msix_num_vectors = num_vectors;

  // Get MSI-X Table address (BAR vaddr + offset)
  // BAR should already be mapped by pci_enable_device
  void __iomem *bar_vaddr = dev->bar[dev->msix_table_bar].vaddr;
  if (!bar_vaddr)
    return -EFAULT;

  volatile uint32_t __iomem *table =
      (volatile uint32_t __iomem *)((uint8_t __iomem *)bar_vaddr +
                                    dev->msix_table_offset);
  uint32_t bsp_apic_id = (uint32_t)(lapic_read(LAPIC_ID) >> 24);

  // Write all Table Entries: masked, with vector numbers
  for (int i = 0; i < num_vectors; i++) {
    uint32_t vector = base + i;
    *(volatile uint32_t __force *)&table[i * 4 + 0] =
        0xFEE00000 | (bsp_apic_id << 12);                // Message Address low
    *(volatile uint32_t __force *)&table[i * 4 + 1] = 0; // Message Address high
    *(volatile uint32_t __force *)&table[i * 4 + 2] =
        vector; // Message Data (vector, Fixed, Edge)
    *(volatile uint32_t __force *)&table[i * 4 + 3] =
        1; // Vector Control: Mask bit = 1
    // Immediate readback at the same vaddr just written, to confirm the write
    // landed in device MMIO (not dropped to a stale/CoW mapping). If this
    // reads back 0, our BAR vaddr does NOT point at the real MSI-X table and
    // every later table read is meaningless.
    if (i == 0)
      printk(LOG_INFO,
             "MSI-X entry0 write-then-read: addrlo=0x%x data=0x%x ctrl=0x%x\n",
             *(volatile uint32_t __force *)&table[i * 4 + 0],
             *(volatile uint32_t __force *)&table[i * 4 + 2],
             *(volatile uint32_t __force *)&table[i * 4 + 3]);
  }

  // Enable MSI-X: set Enable bit (bit 15 of 16-bit Message Control)
  // Also set Function Mask (bit 14) temporarily to prevent spurious interrupts
  // ECAM requires DWORD-aligned access. Message Control is at cap_offset+2
  // (upper 16 bits of the DWORD at cap_offset)
  cap_dword =
      pci_read_config(dev->bus, dev->dev, dev->func, dev->msix_cap_offset);
  msg_ctrl = (cap_dword >> 16) & 0xFFFF;
  msg_ctrl |= (1 << 15); // MSI-X Enable (bit 15 of 16-bit Message Control)
  msg_ctrl |= (1 << 14); // Function Mask (bit 14 of 16-bit Message Control)
  cap_dword = (cap_dword & 0xFFFF) | ((uint32_t)msg_ctrl << 16);
  pci_write_config(dev->bus, dev->dev, dev->func, dev->msix_cap_offset,
                   cap_dword);

  // Disable INTx: set bit 10 (Interrupt Disable) in Command register
  uint32_t cmd = pci_read_config(dev->bus, dev->dev, dev->func, 0x04);
  cmd |= (1 << 10); // Interrupt Disable
  pci_write_config(dev->bus, dev->dev, dev->func, 0x04, cmd);

  // Clear Function Mask now that setup is done
  cap_dword =
      pci_read_config(dev->bus, dev->dev, dev->func, dev->msix_cap_offset);
  msg_ctrl = (cap_dword >> 16) & 0xFFFF;
  msg_ctrl &= ~(1 << 14); // Clear Function Mask
  cap_dword = (cap_dword & 0xFFFF) | ((uint32_t)msg_ctrl << 16);
  pci_write_config(dev->bus, dev->dev, dev->func, dev->msix_cap_offset,
                   cap_dword);

  return num_vectors;
}

__attribute__((no_sanitize("kernel-address"))) void
pci_msix_unmask_entry(pci_device *dev, int entry) {
  if (dev->msix_cap_offset == 0 || entry >= dev->msix_num_vectors)
    return;
  void __iomem *bar_vaddr = dev->bar[dev->msix_table_bar].vaddr;
  volatile uint32_t __iomem *table =
      (volatile uint32_t __iomem *)((uint8_t __iomem *)bar_vaddr +
                                    dev->msix_table_offset);
  *(volatile uint32_t __force *)&table[entry * 4 + 3] &= ~1; // Clear Mask bit
}

// ===================== pci_init =====================

__attribute__((no_sanitize("kernel-address"))) void pci_init() {
  if (g_mcfg.ecam_base == 0)
    return;

  ecam_start_bus = g_mcfg.start_bus;
  ecam_end_bus = g_mcfg.end_bus;

  // 1. Map ECAM configuration space
  map_ecam_mmio(g_mcfg.ecam_base, ecam_start_bus, ecam_end_bus);

  // 2. Scan buses
  pci_scan_bus(ecam_start_bus);
}

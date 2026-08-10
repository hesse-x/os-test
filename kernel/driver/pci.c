/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/driver/pci.h"
#include "arch/x64/apic.h"
#include "arch/x64/memlayout.h" // KERNEL_VMA_BOUNDARY
#include "arch/x64/paging.h"
#include "arch/x64/utils.h"
#include "kernel/xcore/acpi.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/mem/kasan.h"
#include "kernel/xcore/mutex.h"
#include "kernel/xcore/trap.h"
#include "utils/macro.h"
#include "xos/page.h"
#include <stdbool.h>
#include <stddef.h>
#include <xos/errno.h>
#include <xos/syscall_nums.h>

pci_device pci_devices[MAX_PCI_DEV];
int pci_device_count = 0;

void __iomem *ecam_vbase = NULL;
uint8_t ecam_start_bus = 0;
uint8_t ecam_end_bus = 0;

#define PCI_VECTOR_FIRST 64
#define PCI_VECTOR_COUNT 32
static uint32_t pci_vector_bitmap;
static mutex pci_resource_mutex;
static struct pci_resource_stats pci_stats;
static int pci_alloc_vectors(int count);
static void pci_free_vectors(int base, int count);

#define PCI_MAX_DRIVERS 16
static const struct pci_driver *pci_drivers[PCI_MAX_DRIVERS];
static size_t pci_driver_count;
static mutex pci_driver_mutex;
#ifdef TEST
static enum pci_fault_point pci_fault_once;
#endif

static bool pci_fault(enum pci_fault_point point) {
#ifdef TEST
  if (pci_fault_once == point) {
    pci_fault_once = PCI_FAULT_NONE;
    return true;
  }
#else
  (void)point;
#endif
  return false;
}

void pci_test_fail_once(enum pci_fault_point point) {
#ifdef TEST
  pci_fault_once = point;
#else
  (void)point;
#endif
}

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

__attribute__((no_sanitize("kernel-address"))) uint16_t
pci_read_config16(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset) {
  volatile uint16_t __iomem *addr =
      (volatile uint16_t __iomem *)((uint8_t __iomem *)ecam_vbase +
                                    ((uint64_t)bus << 20) +
                                    ((uint64_t)dev << 15) +
                                    ((uint64_t)func << 12) + offset);
  return *(volatile uint16_t __force *)addr;
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

__attribute__((no_sanitize("kernel-address"))) void
pci_write_config16(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset,
                   uint16_t value) {
  volatile uint16_t __iomem *addr =
      (volatile uint16_t __iomem *)((uint8_t __iomem *)ecam_vbase +
                                    ((uint64_t)bus << 20) +
                                    ((uint64_t)dev << 15) +
                                    ((uint64_t)func << 12) + offset);
  *(volatile uint16_t __force *)addr = value;
}

// ===================== BAR sizing =====================

static bool pci_size_bar(pci_device *d, int bar_idx) {
  uint8_t bus = d->bus, dev = d->dev, func = d->func;
  uint8_t offset = 0x10 + bar_idx * 4;

  uint32_t orig = pci_read_config(bus, dev, func, offset);
  if (orig == 0 || orig == 0xFFFFFFFF) {
    d->bar[bar_idx].phys = 0;
    d->bar[bar_idx].size = 0;
    d->bar[bar_idx].type = 0;
    return true;
  }

  uint16_t command = pci_read_config16(bus, dev, func, 0x04);
  pci_write_config16(bus, dev, func, 0x04, command & ~0x3u);
  bool is_io = (orig & PCI_BAR_IO_SPACE);
  bool restored = true;

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
    if (bar_idx == 5) {
      pci_write_config(bus, dev, func, offset, orig);
      pci_write_config16(bus, dev, func, 0x04, command);
      d->bar[bar_idx].phys = 0;
      d->bar[bar_idx].size = 0;
      return false;
    }
    // Also size the high 32 bits (next BAR) before restoring low
    uint32_t orig_hi = pci_read_config(bus, dev, func, offset + 4);
    pci_write_config(bus, dev, func, offset + 4, 0xFFFFFFFF);
    uint32_t mask_hi = pci_read_config(bus, dev, func, offset + 4);
    // Restore both halves
    pci_write_config(bus, dev, func, offset + 4, orig_hi);
    pci_write_config(bus, dev, func, offset, orig);
    restored = pci_read_config(bus, dev, func, offset + 4) == orig_hi;

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
  pci_write_config16(bus, dev, func, 0x04, command);
  if (!restored || pci_read_config(bus, dev, func, offset) != orig ||
      pci_read_config16(bus, dev, func, 0x04) != command) {
    d->bar[bar_idx].phys = 0;
    d->bar[bar_idx].size = 0;
    return false;
  }
  return true;
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
  d->revision_id = rev_class & 0xff;
  d->class_code = class_code;
  d->class_id = (rev_class >> 8) & 0xffffff;
  d->header_type = header_type & 0x7F;
  d->msix_cap_offset = 0;
  d->msi_cap_offset = 0;
  d->msix_table_bar = 0;
  d->msix_pba_bar = 0;
  d->msix_table_offset = 0;
  d->msix_pba_offset = 0;
  d->msix_vector_base = -1;
  d->msix_num_vectors = 0;
  d->irq_mode = PCI_IRQ_NONE;
  d->irq_registered_mask = 0;
  d->irq_state_saved = false;
  d->bar_sizing_valid = true;
  d->enabled = false;
  d->dma_mask = UINT32_MAX;
  d->bind_state = PCI_BIND_UNBOUND;
  d->driver = NULL;
  d->driver_private = NULL;
  for (int i = 0; i < 6; i++) {
    d->bar[i].vaddr = NULL;
    d->bar[i].map_slot = -1;
    d->bar[i].map_page = NULL;
    d->bar[i].map_write_combining = false;
  }
  if ((header_type & 0x7F) == PCI_HEADER_TYPE_NORMAL) {
    uint32_t subsystem = pci_read_config(bus, dev, func, 0x2C);
    d->subsystem_vendor_id = subsystem & 0xffff;
    d->subsystem_device_id = subsystem >> 16;
  } else {
    d->subsystem_vendor_id = 0;
    d->subsystem_device_id = 0;
  }

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
    if (!pci_size_bar(d, i))
      d->bar_sizing_valid = false;
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

__attribute__((no_sanitize("kernel-address"))) void __iomem *
pci_iomap(pci_device *d, int bar_idx, bool write_combining) {
  if (!d || bar_idx < 0 || bar_idx >= 6 || d->bar[bar_idx].size == 0 ||
      d->bar[bar_idx].type == 1)
    return NULL;

  mutex_lock(&pci_resource_mutex);
  pci_bar *bar = &d->bar[bar_idx];
  if (bar->vaddr) {
    void __iomem *vaddr =
        bar->map_write_combining == write_combining ? bar->vaddr : NULL;
    mutex_unlock(&pci_resource_mutex);
    return vaddr;
  }

  if (bar->phys > UINT64_MAX - bar->size) {
    mutex_unlock(&pci_resource_mutex);
    return NULL;
  }
  uint64_t region_start = bar->phys & ~0x1fffffULL;
  uint64_t region_end = ALIGN_UP(bar->phys + bar->size, 0x200000);
  size_t num_2mb = (region_end - region_start) / 0x200000;
  if (region_end < region_start || num_2mb == 0 || num_2mb > 512) {
    mutex_unlock(&pci_resource_mutex);
    return NULL;
  }

  int pdpt_idx = -1;
  if (pci_fault(PCI_FAULT_BAR_SLOT)) {
    mutex_unlock(&pci_resource_mutex);
    return NULL;
  }
  for (int i = 511; i >= 0; i--) {
    if (pdpt_hh[i] == 0) {
      pdpt_idx = i;
      break;
    }
  }
  if (pdpt_idx < 0) {
    mutex_unlock(&pci_resource_mutex);
    return NULL;
  }

  struct page *pd_page =
      pci_fault(PCI_FAULT_BAR_PAGE) ? NULL : bfc_alloc_page(1);
  if (!pd_page) {
    mutex_unlock(&pci_resource_mutex);
    return NULL;
  }
  uint64_t *pd = (__force uint64_t *)phys_to_virt(
      (__force phys_addr_t)page_to_phys(pd_page));
  __memset(pd, 0, PAGE_SIZE);
  uint64_t cache_flags = write_combining ? PTE_WC : PTE_UC;
  for (size_t i = 0; i < num_2mb; i++)
    pd[i] = (region_start + i * 0x200000) | PTE_PRESENT | PTE_RW | PTE_PS |
            cache_flags;

  if (pci_fault(PCI_FAULT_BAR_PUBLISH)) {
    bfc_free_page(pd_page, 1);
    mutex_unlock(&pci_resource_mutex);
    return NULL;
  }

  pdpt_hh[pdpt_idx] =
      (__force uint64_t)page_to_phys(pd_page) | PTE_PRESENT | PTE_RW;
  uint64_t vma =
      (0xffffULL << 48) | (511ULL << 39) | ((uint64_t)pdpt_idx << 30);
  bar->vaddr = (void __iomem __force *)(vma + (bar->phys - region_start));
  bar->map_slot = pdpt_idx;
  bar->map_page = pd_page;
  bar->map_write_combining = write_combining;
  pci_stats.mapped_bars++;
  device_vma_base = vma + num_2mb * 0x200000;
  flush_tlb();
  void __iomem *result = bar->vaddr;
  mutex_unlock(&pci_resource_mutex);
  return result;
}

__attribute__((no_sanitize("kernel-address"))) void pci_iounmap(pci_device *d,
                                                                int bar_idx) {
  if (!d || bar_idx < 0 || bar_idx >= 6)
    return;
  mutex_lock(&pci_resource_mutex);
  pci_bar *bar = &d->bar[bar_idx];
  if (bar->vaddr && bar->map_slot >= 0 && bar->map_page) {
    pdpt_hh[bar->map_slot] = 0;
    flush_tlb();
    bfc_free_page(bar->map_page, 1);
    bar->vaddr = NULL;
    bar->map_slot = -1;
    bar->map_page = NULL;
    bar->map_write_combining = false;
    ASSERT(pci_stats.mapped_bars > 0);
    pci_stats.mapped_bars--;
  }
  mutex_unlock(&pci_resource_mutex);
}

// ===================== Device enablement =====================

__attribute__((no_sanitize("kernel-address"))) int
pci_enable_device_bars(pci_device *d, uint32_t bar_mask, uint32_t wc_mask) {
  if (!d || (bar_mask & ~0x3fu) || (wc_mask & ~bar_mask))
    return -EINVAL;
  uint32_t newly_mapped = 0;
  int max_bars = d->header_type == PCI_HEADER_TYPE_BRIDGE ? 2 : 6;
  for (int i = 0; i < max_bars; i++) {
    if (!(bar_mask & (1u << i)) || d->bar[i].size == 0 || d->bar[i].type == 1)
      continue;
    bool want_wc = (wc_mask & (1u << i)) != 0;
    if (d->bar[i].vaddr && d->bar[i].map_write_combining != want_wc) {
      for (int j = 0; j < max_bars; j++)
        if (newly_mapped & (1u << j))
          pci_iounmap(d, j);
      return -EBUSY;
    }
    if (d->bar[i].vaddr)
      continue;
    if (!pci_iomap(d, i, want_wc)) {
      for (int j = 0; j < max_bars; j++)
        if (newly_mapped & (1u << j))
          pci_iounmap(d, j);
      return -ENOMEM;
    }
    newly_mapped |= 1u << i;
  }

  uint16_t cmd = pci_read_config16(d->bus, d->dev, d->func, 0x04);
  cmd |= (1 << 1) | (1 << 2); // Bus Master + Memory Space
  pci_write_config16(d->bus, d->dev, d->func, 0x04, cmd);
  d->enabled = true;
  return 0;
}

__attribute__((no_sanitize("kernel-address"))) int
pci_enable_device(pci_device *d) {
  return pci_enable_device_bars(d, 0x3f, 0);
}

__attribute__((no_sanitize("kernel-address"))) int
pci_enable_device_wc(pci_device *d, int wc_bar_idx) {
  if (wc_bar_idx < 0 || wc_bar_idx >= 6)
    return -EINVAL;
  return pci_enable_device_bars(d, 0x3f, 1u << wc_bar_idx);
}

__attribute__((no_sanitize("kernel-address"))) void
pci_disable_device(pci_device *d) {
  if (!d)
    return;
  pci_disable_interrupts(d);
  uint16_t cmd = pci_read_config16(d->bus, d->dev, d->func, 0x04);
  cmd &= ~((1u << 1) | (1u << 2));
  pci_write_config16(d->bus, d->dev, d->func, 0x04, cmd);
  for (int i = 0; i < 6; i++)
    pci_iounmap(d, i);
  d->enabled = false;
}

int pci_set_dma_mask(pci_device *d, uint8_t address_bits) {
  if (!d || (address_bits != 32 && address_bits != 64))
    return -EINVAL;
  d->dma_mask = address_bits == 64 ? UINT64_MAX : (uint64_t)UINT32_MAX;
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

static bool pci_id_is_end(const struct pci_device_id *id) {
  return id->vendor == 0 && id->device == 0 && id->subsystem_vendor == 0 &&
         id->subsystem_device == 0 && id->class_id == 0 && id->class_mask == 0;
}

const struct pci_device_id *pci_match_id(const struct pci_device_id *ids,
                                         const pci_device *dev) {
  if (!ids || !dev)
    return NULL;
  for (const struct pci_device_id *id = ids; !pci_id_is_end(id); id++) {
    if (id->vendor != PCI_ANY_ID && id->vendor != dev->vendor_id)
      continue;
    if (id->device != PCI_ANY_ID && id->device != dev->device_id)
      continue;
    if (id->subsystem_vendor != PCI_ANY_ID &&
        id->subsystem_vendor != dev->subsystem_vendor_id)
      continue;
    if (id->subsystem_device != PCI_ANY_ID &&
        id->subsystem_device != dev->subsystem_device_id)
      continue;
    if ((id->class_id & id->class_mask) != (dev->class_id & id->class_mask))
      continue;
    return id;
  }
  return NULL;
}

static int pci_bind_locked(pci_device *dev, const struct pci_driver *driver) {
  if (dev->bind_state != PCI_BIND_UNBOUND)
    return -EBUSY;
  const struct pci_device_id *id = pci_match_id(driver->id_table, dev);
  if (!id)
    return -ENODEV;

  dev->bind_state = PCI_BIND_PROBING;
  dev->driver = driver;
  dev->driver_private = NULL;
  int rc = driver->probe(dev, id);
  if (rc) {
    dev->driver_private = NULL;
    dev->driver = NULL;
    dev->bind_state = PCI_BIND_UNBOUND;
    return rc;
  }
  dev->bind_state = PCI_BIND_BOUND;
  mutex_lock(&pci_resource_mutex);
  pci_stats.bound_devices++;
  mutex_unlock(&pci_resource_mutex);
  return 0;
}

int pci_register_driver(const struct pci_driver *driver) {
  if (!driver || !driver->name || !driver->id_table || !driver->probe)
    return -EINVAL;

  mutex_lock(&pci_driver_mutex);
  for (size_t i = 0; i < pci_driver_count; i++) {
    if (pci_drivers[i] == driver) {
      mutex_unlock(&pci_driver_mutex);
      return -EEXIST;
    }
  }
  if (pci_driver_count == PCI_MAX_DRIVERS) {
    mutex_unlock(&pci_driver_mutex);
    return -ENOSPC;
  }
  if (pci_fault(PCI_FAULT_DRIVER_REGISTER)) {
    mutex_unlock(&pci_driver_mutex);
    return -ENOMEM;
  }
  pci_drivers[pci_driver_count++] = driver;
  for (int i = 0; i < pci_device_count; i++) {
    if (pci_devices[i].bind_state == PCI_BIND_UNBOUND)
      (void)pci_bind_locked(&pci_devices[i], driver);
  }
  mutex_unlock(&pci_driver_mutex);
  return 0;
}

void pci_unregister_driver(const struct pci_driver *driver) {
  if (!driver)
    return;
  mutex_lock(&pci_driver_mutex);
  size_t slot = pci_driver_count;
  for (size_t i = 0; i < pci_driver_count; i++) {
    if (pci_drivers[i] == driver) {
      slot = i;
      break;
    }
  }
  if (slot == pci_driver_count) {
    mutex_unlock(&pci_driver_mutex);
    return;
  }

  for (int i = 0; i < pci_device_count; i++) {
    pci_device *dev = &pci_devices[i];
    if (dev->driver != driver || dev->bind_state != PCI_BIND_BOUND)
      continue;
    dev->bind_state = PCI_BIND_REMOVING;
    if (driver->remove)
      driver->remove(dev);
    mutex_lock(&pci_resource_mutex);
    ASSERT(pci_stats.bound_devices > 0);
    pci_stats.bound_devices--;
    mutex_unlock(&pci_resource_mutex);
    dev->driver_private = NULL;
    dev->driver = NULL;
    dev->bind_state = PCI_BIND_UNBOUND;
  }
  for (size_t i = slot + 1; i < pci_driver_count; i++)
    pci_drivers[i - 1] = pci_drivers[i];
  pci_drivers[--pci_driver_count] = NULL;
  mutex_unlock(&pci_driver_mutex);
}

void pci_set_driver_private(pci_device *dev, void *private_data) {
  if (dev)
    dev->driver_private = private_data;
}

void *pci_get_driver_private(const pci_device *dev) {
  return dev ? dev->driver_private : NULL;
}

#ifdef TEST
static int pci_test_probe_count;
static int pci_test_remove_count;
static bool pci_test_fail_probe;
static int pci_test_cookie;

static void pci_test_irq(trapframe *frame) { (void)frame; }
static void pci_test_irq_ctx(trapframe *frame, void *ctx) {
  (void)frame;
  BUG_ON(ctx != &pci_test_cookie);
}

static int pci_test_probe(pci_device *dev, const struct pci_device_id *id) {
  BUG_ON(!dev || !id || dev->bind_state != PCI_BIND_PROBING);
  pci_test_probe_count++;
  pci_set_driver_private(dev, &pci_test_cookie);
  return pci_test_fail_probe ? -EIO : 0;
}

static void pci_test_remove(pci_device *dev) {
  BUG_ON(!dev || dev->bind_state != PCI_BIND_REMOVING);
  BUG_ON(pci_get_driver_private(dev) != &pci_test_cookie);
  pci_test_remove_count++;
}

static const struct pci_device_id pci_test_ids[] = {
    {
        .vendor = 0xfefe,
        .device = 0x0001,
        .subsystem_vendor = PCI_ANY_ID,
        .subsystem_device = PCI_ANY_ID,
    },
    {0},
};

static const struct pci_driver pci_test_driver = {
    .name = "pci-lifecycle-test",
    .id_table = pci_test_ids,
    .probe = pci_test_probe,
    .remove = pci_test_remove,
};
#endif

void pci_lifecycle_selftest(void) {
#ifdef TEST
  struct pci_resource_stats baseline;
  pci_get_resource_stats(&baseline);
  size_t baseline_free_pages = bfc_free_page_nums();
  BUG_ON(pci_device_count >= MAX_PCI_DEV);
  pci_device *fake = &pci_devices[pci_device_count++];
  __memset(fake, 0, sizeof(*fake));
  fake->vendor_id = 0xfefe;
  fake->device_id = 0x0001;
  fake->class_code = PCI_CLASS_DISPLAY;
  fake->class_id = (uint32_t)PCI_CLASS_DISPLAY << 8;
  fake->bind_state = PCI_BIND_UNBOUND;
  for (int i = 0; i < 6; i++)
    fake->bar[i].map_slot = -1;

  pci_device wrong_id = *fake;
  wrong_id.device_id = 0x0002;
  BUG_ON(pci_match_id(pci_test_ids, &wrong_id) != NULL);

  pci_test_fail_once(PCI_FAULT_DRIVER_REGISTER);
  BUG_ON(pci_register_driver(&pci_test_driver) != -ENOMEM);
  BUG_ON(fake->bind_state != PCI_BIND_UNBOUND || fake->driver != NULL);

  pci_test_probe_count = 0;
  pci_test_remove_count = 0;
  pci_test_fail_probe = true;
  BUG_ON(pci_register_driver(&pci_test_driver) != 0);
  BUG_ON(fake->bind_state != PCI_BIND_UNBOUND || fake->driver != NULL ||
         pci_get_driver_private(fake) != NULL);
  pci_unregister_driver(&pci_test_driver);

  pci_test_fail_probe = false;
  BUG_ON(pci_register_driver(&pci_test_driver) != 0);
  BUG_ON(fake->bind_state != PCI_BIND_BOUND ||
         fake->driver != &pci_test_driver ||
         pci_get_driver_private(fake) != &pci_test_cookie);
  pci_unregister_driver(&pci_test_driver);
  BUG_ON(fake->bind_state != PCI_BIND_UNBOUND || fake->driver != NULL ||
         pci_get_driver_private(fake) != NULL);
  BUG_ON(pci_test_probe_count != 2 || pci_test_remove_count != 1);

  int vector = pci_alloc_vectors(1);
  BUG_ON(vector < PCI_VECTOR_FIRST);
  pci_free_vectors(vector, 1);
  pci_test_fail_once(PCI_FAULT_VECTOR_ALLOC);
  BUG_ON(pci_alloc_vectors(1) != -ENOMEM);

  int irq_vector = pci_alloc_vectors(1);
  BUG_ON(irq_vector < PCI_VECTOR_FIRST);
  fake->irq_mode = PCI_IRQ_MSI;
  fake->msix_vector_base = irq_vector;
  fake->msix_num_vectors = 1;
  pci_test_fail_once(PCI_FAULT_IRQ_REGISTER);
  BUG_ON(pci_request_irq(fake, 0, pci_test_irq) != -ENOMEM);
  BUG_ON(fake->irq_registered_mask != 0 || irq_has_handler(irq_vector));
  BUG_ON(pci_request_irq_ctx(fake, 0, pci_test_irq_ctx, &pci_test_cookie) != 0);
  BUG_ON(!(fake->irq_registered_mask & 1u) || !irq_has_handler(irq_vector));
  pci_free_irq(fake, 0);
  BUG_ON(fake->irq_registered_mask != 0 || irq_has_handler(irq_vector));
  fake->irq_mode = PCI_IRQ_NONE;
  fake->msix_vector_base = -1;
  fake->msix_num_vectors = 0;
  pci_free_vectors(irq_vector, 1);

  fake->bar[0].phys = 0x100000;
  fake->bar[0].size = PAGE_SIZE;
  const enum pci_fault_point bar_faults[] = {
      PCI_FAULT_BAR_SLOT,
      PCI_FAULT_BAR_PAGE,
      PCI_FAULT_BAR_PUBLISH,
  };
  for (size_t i = 0; i < sizeof(bar_faults) / sizeof(bar_faults[0]); i++) {
    pci_test_fail_once(bar_faults[i]);
    BUG_ON(pci_iomap(fake, 0, false) != NULL);
    BUG_ON(fake->bar[0].vaddr || fake->bar[0].map_slot != -1 ||
           fake->bar[0].map_page);
    BUG_ON(bfc_free_page_nums() != baseline_free_pages);
  }
  BUG_ON(!pci_iomap(fake, 0, false));
  BUG_ON(fake->bar[0].map_slot < 0 || !fake->bar[0].map_page);
  pci_iounmap(fake, 0);
  BUG_ON(fake->bar[0].vaddr || fake->bar[0].map_slot != -1 ||
         fake->bar[0].map_page);
  struct pci_resource_stats after;
  pci_get_resource_stats(&after);
  BUG_ON(after.mapped_bars != baseline.mapped_bars ||
         after.allocated_vectors != baseline.allocated_vectors ||
         after.registered_irqs != baseline.registered_irqs ||
         after.bound_devices != baseline.bound_devices);
  BUG_ON(bfc_free_page_nums() != baseline_free_pages);
  __memset(fake, 0, sizeof(*fake));
  pci_device_count--;
  printk(LOG_INFO,
         "pci lifecycle selftest: PASS (failure unwind, controlled remove)\n");
#endif
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
  if (!ptr || ptr >= KERNEL_VMA_BOUNDARY ||
      ptr + sizeof(struct pci_dev_info) > KERNEL_VMA_BOUNDARY)
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

static int pci_alloc_vectors(int count) {
  if (count <= 0 || count > PCI_VECTOR_COUNT)
    return -EINVAL;
  mutex_lock(&pci_resource_mutex);
  if (pci_fault(PCI_FAULT_VECTOR_ALLOC)) {
    mutex_unlock(&pci_resource_mutex);
    return -ENOMEM;
  }
  for (int start = 0; start <= PCI_VECTOR_COUNT - count; start++) {
    uint32_t mask = count == 32 ? UINT32_MAX : ((1u << count) - 1u) << start;
    if (!(pci_vector_bitmap & mask)) {
      pci_vector_bitmap |= mask;
      pci_stats.allocated_vectors += count;
      mutex_unlock(&pci_resource_mutex);
      return PCI_VECTOR_FIRST + start;
    }
  }
  mutex_unlock(&pci_resource_mutex);
  return -ENOMEM;
}

static void pci_free_vectors(int base, int count) {
  if (base < PCI_VECTOR_FIRST || count <= 0 ||
      base + count > PCI_VECTOR_FIRST + PCI_VECTOR_COUNT)
    return;
  int start = base - PCI_VECTOR_FIRST;
  uint32_t mask = count == 32 ? UINT32_MAX : ((1u << count) - 1u) << start;
  mutex_lock(&pci_resource_mutex);
  ASSERT((pci_vector_bitmap & mask) == mask);
  ASSERT(pci_stats.allocated_vectors >= (uint32_t)count);
  pci_vector_bitmap &= ~mask;
  pci_stats.allocated_vectors -= count;
  mutex_unlock(&pci_resource_mutex);
}

__attribute__((no_sanitize("kernel-address"))) int
pci_enable_msi(pci_device *dev) {
  if (!dev || dev->irq_mode != PCI_IRQ_NONE)
    return -EBUSY;
  if (dev->msi_cap_offset == 0)
    return -ENOSYS;

  // Read Message Control (upper 16 bits of DWORD at cap_offset)
  uint32_t cap_dword =
      pci_read_config(dev->bus, dev->dev, dev->func, dev->msi_cap_offset);
  uint16_t msg_ctrl = (cap_dword >> 16) & 0xFFFF;
  bool is_64bit = (msg_ctrl & (1 << 7)) != 0; // 64-bit Address Capable

  int vector = pci_alloc_vectors(1);
  if (vector < 0)
    return vector;
  dev->msix_vector_base = vector; // reuse field for MSI vector base
  dev->msix_num_vectors = 1;
  dev->irq_mode = PCI_IRQ_MSI;
  dev->irq_saved_control = msg_ctrl;
  dev->irq_saved_command =
      pci_read_config16(dev->bus, dev->dev, dev->func, 0x04);
  dev->irq_saved_address_lo =
      pci_read_config(dev->bus, dev->dev, dev->func, dev->msi_cap_offset + 4);
  dev->irq_saved_64bit = is_64bit;
  dev->irq_saved_address_hi =
      is_64bit ? pci_read_config(dev->bus, dev->dev, dev->func,
                                 dev->msi_cap_offset + 8)
               : 0;
  dev->irq_saved_data = pci_read_config(
      dev->bus, dev->dev, dev->func, dev->msi_cap_offset + (is_64bit ? 12 : 8));
  dev->irq_state_saved = true;

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
  pci_write_config16(dev->bus, dev->dev, dev->func, 0x04,
                     dev->irq_saved_command | (1 << 10));

  return 0;
}

// ===================== MSI-X =====================

__attribute__((no_sanitize("kernel-address"))) int
pci_enable_msix(pci_device *dev, int num_vectors) {
  if (!dev || dev->irq_mode != PCI_IRQ_NONE)
    return -EBUSY;
  if (dev->msix_cap_offset == 0)
    return -ENOSYS;
  if (num_vectors <= 0)
    return -EINVAL;

  // Read Message Control to get table size (bits 10:2 of 16-bit Message Control
  // = N-1) Message Control is at cap_offset+2, upper 16 bits of DWORD at
  // cap_offset
  uint32_t cap_dword =
      pci_read_config(dev->bus, dev->dev, dev->func, dev->msix_cap_offset);
  uint16_t msg_ctrl = (cap_dword >> 16) & 0xFFFF;
  int table_size = (msg_ctrl & 0x7FF) + 1;
  if (num_vectors > table_size)
    num_vectors = table_size;

  // Get MSI-X Table address (BAR vaddr + offset)
  // BAR should already be mapped by pci_enable_device
  void __iomem *bar_vaddr = dev->bar[dev->msix_table_bar].vaddr;
  if (!bar_vaddr)
    return -EFAULT;
  uint64_t table_bytes = (uint64_t)num_vectors * 16;
  if (dev->msix_table_offset > dev->bar[dev->msix_table_bar].size ||
      table_bytes > dev->bar[dev->msix_table_bar].size - dev->msix_table_offset)
    return -EINVAL;

  int base = pci_alloc_vectors(num_vectors);
  if (base < 0)
    return base;
  dev->msix_vector_base = base;
  dev->msix_num_vectors = num_vectors;
  dev->irq_mode = PCI_IRQ_MSIX;
  dev->irq_saved_control = msg_ctrl;
  dev->irq_saved_command =
      pci_read_config16(dev->bus, dev->dev, dev->func, 0x04);
  dev->irq_state_saved = true;

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
  pci_write_config16(dev->bus, dev->dev, dev->func, 0x04,
                     dev->irq_saved_command | (1 << 10));

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
pci_msix_mask_entry(pci_device *dev, int entry) {
  if (!dev || dev->irq_mode != PCI_IRQ_MSIX || entry < 0 ||
      entry >= dev->msix_num_vectors)
    return;
  void __iomem *bar_vaddr = dev->bar[dev->msix_table_bar].vaddr;
  if (!bar_vaddr)
    return;
  volatile uint32_t __iomem *table =
      (volatile uint32_t __iomem *)((uint8_t __iomem *)bar_vaddr +
                                    dev->msix_table_offset);
  *(volatile uint32_t __force *)&table[entry * 4 + 3] |= 1;
}

__attribute__((no_sanitize("kernel-address"))) void
pci_msix_unmask_entry(pci_device *dev, int entry) {
  if (!dev || dev->irq_mode != PCI_IRQ_MSIX || entry < 0 ||
      entry >= dev->msix_num_vectors)
    return;
  void __iomem *bar_vaddr = dev->bar[dev->msix_table_bar].vaddr;
  volatile uint32_t __iomem *table =
      (volatile uint32_t __iomem *)((uint8_t __iomem *)bar_vaddr +
                                    dev->msix_table_offset);
  *(volatile uint32_t __force *)&table[entry * 4 + 3] &= ~1; // Clear Mask bit
}

int pci_request_irq(pci_device *dev, int entry, void (*handler)(trapframe *)) {
  if (!dev || !handler || dev->irq_mode == PCI_IRQ_NONE || entry < 0 ||
      entry >= dev->msix_num_vectors || entry >= 32)
    return -EINVAL;
  int vector = dev->msix_vector_base + entry;
  mutex_lock(&pci_resource_mutex);
  uint32_t mask = 1u << entry;
  if ((dev->irq_registered_mask & mask) || irq_has_handler(vector)) {
    mutex_unlock(&pci_resource_mutex);
    return -EBUSY;
  }
  if (pci_fault(PCI_FAULT_IRQ_REGISTER)) {
    mutex_unlock(&pci_resource_mutex);
    return -ENOMEM;
  }
  irq_register(vector, handler);
  dev->irq_registered_mask |= mask;
  pci_stats.registered_irqs++;
  mutex_unlock(&pci_resource_mutex);
  return 0;
}

int pci_request_irq_ctx(pci_device *dev, int entry, pci_irq_handler_t handler,
                        void *ctx) {
  if (!dev || !handler || dev->irq_mode == PCI_IRQ_NONE || entry < 0 ||
      entry >= dev->msix_num_vectors || entry >= 32)
    return -EINVAL;
  int vector = dev->msix_vector_base + entry;
  mutex_lock(&pci_resource_mutex);
  uint32_t mask = 1u << entry;
  if ((dev->irq_registered_mask & mask) || irq_has_handler(vector)) {
    mutex_unlock(&pci_resource_mutex);
    return -EBUSY;
  }
  if (pci_fault(PCI_FAULT_IRQ_REGISTER)) {
    mutex_unlock(&pci_resource_mutex);
    return -ENOMEM;
  }
  int rc = irq_register_ctx(vector, handler, ctx);
  if (rc) {
    mutex_unlock(&pci_resource_mutex);
    return rc;
  }
  dev->irq_registered_mask |= mask;
  pci_stats.registered_irqs++;
  mutex_unlock(&pci_resource_mutex);
  return 0;
}

void pci_free_irq(pci_device *dev, int entry) {
  if (!dev || entry < 0 || entry >= 32)
    return;
  mutex_lock(&pci_resource_mutex);
  uint32_t mask = 1u << entry;
  if (dev->irq_registered_mask & mask) {
    irq_unregister_sync(dev->msix_vector_base + entry);
    dev->irq_registered_mask &= ~mask;
    ASSERT(pci_stats.registered_irqs > 0);
    pci_stats.registered_irqs--;
  }
  mutex_unlock(&pci_resource_mutex);
}

__attribute__((no_sanitize("kernel-address"))) void
pci_disable_interrupts(pci_device *dev) {
  if (!dev || dev->irq_mode == PCI_IRQ_NONE)
    return;

  enum pci_irq_mode old_mode = dev->irq_mode;
  if (old_mode == PCI_IRQ_MSIX) {
    uint32_t cap =
        pci_read_config(dev->bus, dev->dev, dev->func, dev->msix_cap_offset);
    uint16_t ctrl = cap >> 16;
    ctrl |= 1u << 14;
    ctrl &= ~(1u << 15);
    pci_write_config(dev->bus, dev->dev, dev->func, dev->msix_cap_offset,
                     (cap & 0xffff) | ((uint32_t)ctrl << 16));
    for (int i = 0; i < dev->msix_num_vectors; i++)
      pci_msix_mask_entry(dev, i);
  } else {
    uint32_t cap =
        pci_read_config(dev->bus, dev->dev, dev->func, dev->msi_cap_offset);
    uint16_t ctrl = cap >> 16;
    ctrl &= ~1u;
    pci_write_config(dev->bus, dev->dev, dev->func, dev->msi_cap_offset,
                     (cap & 0xffff) | ((uint32_t)ctrl << 16));
  }

  for (int i = 0; i < dev->msix_num_vectors; i++) {
    if (dev->irq_registered_mask & (1u << i))
      pci_free_irq(dev, i);
    else
      irq_unregister_sync(dev->msix_vector_base + i);
  }
  pci_free_vectors(dev->msix_vector_base, dev->msix_num_vectors);
  if (dev->irq_state_saved) {
    uint8_t cap_offset =
        old_mode == PCI_IRQ_MSIX ? dev->msix_cap_offset : dev->msi_cap_offset;
    uint32_t cap = pci_read_config(dev->bus, dev->dev, dev->func, cap_offset);
    if (old_mode == PCI_IRQ_MSI) {
      pci_write_config(dev->bus, dev->dev, dev->func, cap_offset + 4,
                       dev->irq_saved_address_lo);
      if (dev->irq_saved_64bit)
        pci_write_config(dev->bus, dev->dev, dev->func, cap_offset + 8,
                         dev->irq_saved_address_hi);
      pci_write_config(dev->bus, dev->dev, dev->func,
                       cap_offset + (dev->irq_saved_64bit ? 12 : 8),
                       dev->irq_saved_data);
    }
    pci_write_config(dev->bus, dev->dev, dev->func, cap_offset,
                     (cap & 0xffff) | ((uint32_t)dev->irq_saved_control << 16));
    pci_write_config16(dev->bus, dev->dev, dev->func, 0x04,
                       dev->irq_saved_command);
    dev->irq_state_saved = false;
  }
  dev->msix_vector_base = -1;
  dev->msix_num_vectors = 0;
  dev->irq_mode = PCI_IRQ_NONE;
}

void pci_get_resource_stats(struct pci_resource_stats *stats) {
  if (!stats)
    return;
  mutex_lock(&pci_resource_mutex);
  *stats = pci_stats;
  mutex_unlock(&pci_resource_mutex);
}

// ===================== pci_init =====================

__attribute__((no_sanitize("kernel-address"))) void pci_init() {
  mutex_init(&pci_driver_mutex);
  mutex_init(&pci_resource_mutex);
  pci_vector_bitmap = 0;
  __memset(&pci_stats, 0, sizeof(pci_stats));
  if (g_mcfg.ecam_base == 0)
    return;

  ecam_start_bus = g_mcfg.start_bus;
  ecam_end_bus = g_mcfg.end_bus;

  // 1. Map ECAM configuration space
  map_ecam_mmio(g_mcfg.ecam_base, ecam_start_bus, ecam_end_bus);

  // 2. Scan buses
  pci_scan_bus(ecam_start_bus);

  // 3. Debug: dump enumerated PCI devices (vendor/device/class) to diagnose
  // driver matching. Temporary.
  for (int i = 0; i < pci_device_count; i++) {
    pci_device *d = &pci_devices[i];
    printk(LOG_INFO, "pci[%d]: %02x:%02x.%02x vid=%04x did=%04x class=%04x\n",
           i, d->bus, d->dev, d->func, d->vendor_id, d->device_id,
           d->class_code);
  }
}

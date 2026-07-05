/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/driver/display.h"
#include "arch/x64/memlayout.h"
#include "arch/x64/paging.h"
#include "arch/x64/utils.h"
#include "kernel/bsd/devtmpfs.h"
#include "kernel/driver/pci.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/mm_types.h"
#include "kernel/xcore/xtask.h"
#include <xos/display.h>
#include <xos/errno.h>
#include <xos/font_metrics.h>
#include <xos/ioctl.h>
#ifdef PERF
#include "arch/x64/apic.h"
#include "kernel/driver/serial.h"
#endif

// Global display state
struct display_state g_display;

// KMS device ops (driver_pid=0 means kernel device)
static struct dev_ops kms_dev_ops = {
    .driver_pid = 0,
    .is_block = false,
    .ioctl = display_ioctl,
    .mmap = display_mmap_handler_ioctl,
};

// ===================== display_init =====================
// PCI discovery + VBE modeset + BAR mapping. No boot_info dependency.

__attribute__((no_sanitize("kernel-address"))) void display_init(void) {
  // 1. PCI discovery: find bochs-display by vendor/device ID
  // (class code is 0x0380 "Display/Other", not 0x0300 VGA)
  pci_device_t *dev = pci_find_device_by_id(0x1234, 0x1111);
  if (!dev) {
    printk(LOG_WARN, "display_init: no display device found\n");
    halt();
  }

  printk(LOG_INFO, "display_init: found display vendor=%x device=%x class=%x\n",
         dev->vendor_id, dev->device_id, dev->class_code);

  // 2. Enable device: framebuffer BAR mapped WC, other BARs UC
  int fb_bar_idx = -1;
  uint64_t max_size = 0;
  for (int i = 0; i < 6; i++) {
    if (dev->bar[i].size > max_size && dev->bar[i].type != 1) {
      max_size = dev->bar[i].size;
      fb_bar_idx = i;
    }
  }
  int rc = pci_enable_device_wc(dev, fb_bar_idx);
  if (rc) {
    printk(LOG_ERROR, "display_init: pci_enable_device_wc failed\n");
    halt();
  }
  printk(LOG_INFO, "display_init: framebuffer BAR%d mapped WC\n", fb_bar_idx);

  // 3. Identify BARs: bochs-display has BAR0=framebuffer (large MMIO),
  //    BAR2=VBE MMIO registers (small MMIO). BAR1 consumed by 64-bit BAR0.
  //    Walk BARs to find framebuffer (largest) and VBE MMIO (has DISPI ID).
  uint8_t __iomem *fb_vaddr = NULL;
  uint16_t __iomem *vbe_mmio = NULL;
  uint64_t fb_size = 0;

  for (int i = 0; i < 6; i++) {
    printk(LOG_INFO, "  BAR%d: phys=%lx size=%lx type=%x vaddr=%p\n", i,
           dev->bar[i].phys, dev->bar[i].size, dev->bar[i].type,
           dev->bar[i].vaddr);
    if (dev->bar[i].size == 0)
      continue;
    if (dev->bar[i].type == 1)
      continue; // skip I/O BAR

    // Try to read VBE DISPI ID from small BARs (< 8KB)
    // QEMU bochs-display: VBE registers at BAR2 offset 0x500
    // (PCI_VGA_BOCHS_OFFSET)
    if (dev->bar[i].size <= 0x2000 && dev->bar[i].vaddr) {
      uint8_t __iomem *mmio_base = (uint8_t __iomem *)dev->bar[i].vaddr;
      uint16_t id = mmio_read16(
          (uint16_t __iomem *)(mmio_base +
                               VBE_DISPI_MMIO_OFFSET(VBE_DISPI_INDEX_ID)));
      if (id == VBE_DISPI_ID_VERSION) {
        vbe_mmio = (uint16_t __iomem *)mmio_base;
        printk(LOG_INFO, "display_init: VBE MMIO at BAR%d, ID=%x\n", i, id);
      }
    }

    // Largest MMIO BAR is the framebuffer
    if (dev->bar[i].size > fb_size) {
      fb_vaddr = (uint8_t __iomem *)dev->bar[i].vaddr;
      fb_size = dev->bar[i].size;
    }
  }

  if (!vbe_mmio) {
    printk(LOG_WARN, "display_init: VBE MMIO BAR not found\n");
    halt();
  }
  printk(LOG_DEBUG, "fb_vaddr: %p\n", fb_vaddr);
  if (!fb_vaddr) {
    printk(LOG_ERROR, "?????????\n");
    printk(LOG_ERROR, "display_init: framebuffer BAR not found\n");
    halt();
  }

  // 4. VBE modeset: 800x600x32
  uint8_t __iomem *mmio = (uint8_t __iomem *)vbe_mmio;
  mmio_write16((uint16_t __iomem *)(mmio + VBE_DISPI_MMIO_OFFSET(
                                               VBE_DISPI_INDEX_ENABLE)),
               0); // disable first
  mmio_write16(
      (uint16_t __iomem *)(mmio + VBE_DISPI_MMIO_OFFSET(VBE_DISPI_INDEX_XRES)),
      800);
  mmio_write16(
      (uint16_t __iomem *)(mmio + VBE_DISPI_MMIO_OFFSET(VBE_DISPI_INDEX_YRES)),
      600);
  mmio_write16(
      (uint16_t __iomem *)(mmio + VBE_DISPI_MMIO_OFFSET(VBE_DISPI_INDEX_BPP)),
      32);
  mmio_write16((uint16_t __iomem *)(mmio + VBE_DISPI_MMIO_OFFSET(
                                               VBE_DISPI_INDEX_VIRT_WIDTH)),
               800);
  mmio_write16((uint16_t __iomem *)(mmio + VBE_DISPI_MMIO_OFFSET(
                                               VBE_DISPI_INDEX_X_OFFSET)),
               0);
  mmio_write16((uint16_t __iomem *)(mmio + VBE_DISPI_MMIO_OFFSET(
                                               VBE_DISPI_INDEX_Y_OFFSET)),
               0);
  mmio_write16((uint16_t __iomem *)(mmio + VBE_DISPI_MMIO_OFFSET(
                                               VBE_DISPI_INDEX_ENABLE)),
               VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

  // 5. Fill g_display
  g_display.front_fb = fb_vaddr;
  g_display.vbe_mmio = vbe_mmio;
  g_display.pci_dev = dev;
  g_display.fb_width = 800;
  g_display.fb_height = 600;
  g_display.fb_pitch = 800 * 4; // 32bpp = 4 bytes per pixel
  g_display.fb_bpp = 32;
  g_display.fb_size = 800 * 4 * 600;
  g_display.fb_rows = 600 / FONT_HEIGHT;

  printk(LOG_INFO, "display_init: 800x600x32 done\n");
}

// ===================== display_ioctl =====================
// Converts (cmd, arg) ioctl pattern to display_req_handler logic.
// Phase 1: called by sys_dev_req via ops->ioctl, arg points to kernel stack
// buffer.

long display_ioctl(uint32_t cmd, void *arg) {
  bool is_create =
      (cmd == KMS_IOCTL_CREATE_BUF || cmd == DISPLAY_REQ_CREATE_BUF);
  bool is_flip = (_IOC_NR(cmd) == 2 || cmd == DISPLAY_REQ_FLIP);

  if (is_create) {
    // Use unified struct layout: input at offsets 0-11, output at offsets 12-31
    struct display_ioctl_create_buf_arg *uarg =
        (struct display_ioctl_create_buf_arg *)arg;
    uint32_t width = uarg->width;
    uint32_t height = uarg->height;
    uint32_t bpp = uarg->bpp;

    // Validate parameters
    if (width != 800 || height != 600 || bpp != 32)
      return -EINVAL;

    // Already initialized
    if (g_display.initialized)
      return -EBUSY;

    // Allocate back buffer
    uint32_t pitch = width * 4;
    uint32_t size = pitch * height;
    size_t npages = (size + 4095) / 4096;

    Page *pages = bfc_alloc_page(npages);
    if (!pages)
      return -ENOMEM;

    uint64_t phys = (__force uint64_t)page_to_phys(pages);
    uint8_t *vaddr = (__force uint8_t *)phys_to_virt((__force phys_addr_t)phys);
    __memset(vaddr, 0, npages * PAGE_SIZE);

    g_display.back_buffer = vaddr;
    g_display.back_buffer_phys = phys;
    g_display.back_buffer_npages = npages;
    g_display.initialized = true;

    // Fill output fields in unified struct (at offsets 12-31)
    uarg->pitch = pitch;
    uarg->size = size;
    uarg->rows = height / FONT_HEIGHT;
    uarg->cols = width / FONT_WIDTH;
    uarg->result = 0;
    return 0;

  } else if (is_flip) {
    if (!g_display.initialized)
      return -ENOENT;

    struct display_ioctl_flip_arg *farg = (struct display_ioctl_flip_arg *)arg;
    uint32_t row_start = farg->dirty_row_start;
    uint32_t row_end = farg->dirty_row_end;
    uint32_t rows = g_display.fb_rows;
    uint32_t pitch = g_display.fb_pitch;

    // Full-frame fallback: invalid range or legacy no-arg call
    if (row_start >= row_end || row_end > rows) {
      row_start = 0;
      row_end = rows;
    }

#ifdef PERF
    uint32_t copy_rows = row_end - row_start;
    uint32_t copy_bytes = copy_rows * FONT_HEIGHT * pitch;
    uint64_t t0 = rdtsc64();
#endif

    if (row_start == 0 && row_end == rows) {
      // Full frame copy
      __memcpy((void __force *)g_display.front_fb, g_display.back_buffer,
               g_display.fb_size);
    } else {
      // Row-level copy
      uint32_t y_start = row_start * FONT_HEIGHT;
      uint32_t y_end = row_end * FONT_HEIGHT;
      __memcpy((void __force *)(g_display.front_fb + y_start * pitch),
               g_display.back_buffer + y_start * pitch,
               (y_end - y_start) * pitch);
    }

#ifdef PERF
    uint64_t t1 = rdtsc64();
    uint64_t delta = t1 - t0;
    uint64_t us = delta / (tsc_per_ms / 1000);
    serial_printf("flip: rows=%u/%u bytes=%u tsc=%lu us=%lu\n", copy_rows, rows,
                  copy_bytes, delta, us);
#endif
    return 0;
  }

  return -EINVAL;
}

__attribute__((no_sanitize("kernel-address"))) int
display_req_handler(uint32_t req_type, void *req_data, uint32_t req_len,
                    void *resp_data, uint32_t resp_len) {
  if (req_type == DISPLAY_REQ_CREATE_BUF) {
    if (req_len < sizeof(struct display_create_buf_req) ||
        resp_len < sizeof(struct display_create_buf_resp))
      return -EINVAL;

    // Construct unified ioctl arg from legacy req, call display_ioctl
    struct display_ioctl_create_buf_arg arg = {0};
    struct display_create_buf_req *req =
        (struct display_create_buf_req *)req_data;
    arg.width = req->width;
    arg.height = req->height;
    arg.bpp = req->bpp;

    long rc = display_ioctl(KMS_IOCTL_CREATE_BUF, &arg);
    if (rc < 0)
      return rc;

    // Copy result from unified arg to legacy resp
    struct display_create_buf_resp *resp =
        (struct display_create_buf_resp *)resp_data;
    resp->pitch = arg.pitch;
    resp->size = arg.size;
    resp->rows = arg.rows;
    resp->cols = arg.cols;
    resp->result = arg.result;
    return 0;

  } else if (req_type == DISPLAY_REQ_FLIP) {
    if (resp_len < sizeof(struct display_flip_resp))
      return -EINVAL;

    struct display_ioctl_flip_arg flip_arg = {0};
    long rc = display_ioctl(KMS_IOCTL_FLIP, &flip_arg);
    struct display_flip_resp *resp = (struct display_flip_resp *)resp_data;
    resp->result = (rc < 0) ? rc : 0;
    return rc;
  }

  return -EINVAL;
}

// ===================== display_mmap_handler_ioctl =====================
// Wrapper to adapt display_mmap_handler to dev_ops.mmap signature (uint64_t
// size)

uint64_t display_mmap_handler_ioctl(xtask_t *proc, uint64_t size) {
  return display_mmap_handler(proc, (size_t)size);
}

// ===================== display_mmap_handler =====================

__attribute__((no_sanitize("kernel-address"))) uint64_t
display_mmap_handler(xtask_t *proc, size_t size) {
  if (!g_display.initialized)
    return 0;

  // Map back buffer physical pages into user address space
  size_t npages = g_display.back_buffer_npages;
  uint64_t *pml4 =
      (__force uint64_t *)phys_to_virt((__force phys_addr_t)proc->mm->cr3);
  uint64_t vaddr = proc->mm->mmap_brk;
  uint64_t pte_flags = PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX;

  for (size_t i = 0; i < npages; i++) {
    uint64_t page_phys = g_display.back_buffer_phys + i * PAGE_SIZE;
    if (!map_user_page_direct(pml4, vaddr + i * PAGE_SIZE, page_phys,
                              pte_flags)) {
      // Cleanup on failure
      for (size_t j = 0; j < i; j++)
        unmap_user_pages(pml4, vaddr + j * PAGE_SIZE,
                         vaddr + (j + 1) * PAGE_SIZE, 1);
      return 0;
    }
  }

  // Create mmap_region_t for proc_reap cleanup
  mmap_region_t *region = (mmap_region_t *)kmalloc(sizeof(mmap_region_t));
  if (!region) {
    for (size_t i = 0; i < npages; i++)
      unmap_user_pages(pml4, vaddr + i * PAGE_SIZE, vaddr + (i + 1) * PAGE_SIZE,
                       1);
    return 0;
  }

  region->vaddr = vaddr;
  region->size = npages * PAGE_SIZE;
  region->phys =
      g_display.back_buffer_phys; // Mark as externally-managed physical mapping
  region->shm_obj = NULL;
  region->next = proc->mm->mmap_regions;
  proc->mm->mmap_regions = region;
  proc->mm->mmap_brk = vaddr + npages * PAGE_SIZE;

  return vaddr;
}

// ===================== display_dev_register =====================

void display_dev_register(void) {
  int rc = devtmpfs_create("kms", &kms_dev_ops, NULL);
  if (rc != 0) {
    printk(LOG_ERROR, "display_dev_register: failed (rc=%d)\n", rc);
  } else {
    printk(LOG_INFO, "display_dev_register: /dev/kms registered\n");
  }
}

// ===================== Driver registry =====================
#include "kernel/driver/driver.h"

dev_driver_t display_driver = {
    .name = "display",
    .pci_class = 0, // Matched by vendor/device ID (bochs-display class=0x0380)
    .pci_vendor = 0x1234, // QEMU bochs-display
    .pci_device = 0x1111,
    .init = display_init,
    .ops = &kms_dev_ops,
};

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stddef.h>

#include "arch/x64/utils.h"
#include "kernel/driver/pci.h"
#include "kernel/driver/virtio_pci.h"
#include "kernel/xcore/log.h"

/* Read a virtio_pci_cap from PCI config space cap chain.
   Returns true if found, fills cap struct. */
static bool virtio_pci_find_cap(struct pci_device *pdev, uint8_t cfg_type,
                                struct virtio_pci_cap *out) {
  /* Walk cap chain starting at offset 0x34 (cap pointer in config header) */
  uint8_t cap_off =
      (uint8_t)(pci_read_config(pdev->bus, pdev->dev, pdev->func, 0x34) & 0xFF);
  while (cap_off != 0) {
    uint32_t hdr0 = pci_read_config(pdev->bus, pdev->dev, pdev->func, cap_off);
    uint8_t cap_vndr = hdr0 & 0xFF;
    uint8_t cap_next = (hdr0 >> 8) & 0xFF;
    uint8_t cap_len = (hdr0 >> 16) & 0xFF;
    if (cap_vndr == PCI_CAP_ID_VNDR) {
      uint8_t this_cfg_type = (hdr0 >> 24) & 0xFF;
      /* Read remaining fields: bar, offset, length (at cap_off + 4, +8, +12) */
      uint32_t hdr1 =
          pci_read_config(pdev->bus, pdev->dev, pdev->func, cap_off + 4);
      uint32_t hdr2 =
          pci_read_config(pdev->bus, pdev->dev, pdev->func, cap_off + 8);
      uint8_t bar = hdr1 & 0xFF;
      uint32_t offset = hdr2 & 0xFFFFFFFF;
      uint32_t length =
          pci_read_config(pdev->bus, pdev->dev, pdev->func, cap_off + 12);
      if (this_cfg_type == cfg_type) {
        out->cap_vndr = cap_vndr;
        out->cap_next = cap_next;
        out->cap_len = cap_len;
        out->cfg_type = this_cfg_type;
        out->bar = bar;
        out->offset = offset;
        out->length = length;
        return true;
      }
    }
    cap_off = cap_next;
  }
  return false;
}

/* Get the kernel virtual address of a capability region (BAR vaddr + offset) */
static void __iomem *virtio_pci_cap_addr(struct pci_device *pdev,
                                         struct virtio_pci_cap *cap) {
  if (cap->bar >= 6)
    return NULL;
  void __iomem *bar_vaddr = pdev->bar[cap->bar].vaddr;
  if (!bar_vaddr)
    return NULL;
  return (void __iomem *)((uint8_t __iomem *)bar_vaddr + cap->offset);
}

int virtio_pci_init(struct virtio_pci_dev *vdev, struct pci_device *pdev) {
  __memset(vdev, 0, sizeof(*vdev));
  vdev->pdev = pdev;

  /* Enable device (maps all BARs) */
  if (pci_enable_device(pdev) < 0) {
    printk(LOG_ERROR, "virtio_pci: pci_enable_device failed\n");
    return -1;
  }

  /* Find and map capabilities */
  struct virtio_pci_cap cap_common, cap_notify, cap_isr, cap_dev;
  if (!virtio_pci_find_cap(pdev, VIRTIO_PCI_CAP_COMMON_CFG, &cap_common)) {
    printk(LOG_ERROR, "virtio_pci: common_cfg cap not found\n");
    return -1;
  }
  if (!virtio_pci_find_cap(pdev, VIRTIO_PCI_CAP_NOTIFY_CFG, &cap_notify)) {
    printk(LOG_ERROR, "virtio_pci: notify cap not found\n");
    return -1;
  }
  if (!virtio_pci_find_cap(pdev, VIRTIO_PCI_CAP_ISR_CFG, &cap_isr)) {
    printk(LOG_ERROR, "virtio_pci: isr cap not found\n");
    return -1;
  }
  /* device_cfg is optional for some devices but required for virtio-gpu */
  bool has_dev_cfg =
      virtio_pci_find_cap(pdev, VIRTIO_PCI_CAP_DEVICE_CFG, &cap_dev);

  vdev->common = (struct virtio_pci_common_cfg __iomem *)virtio_pci_cap_addr(
      pdev, &cap_common);
  vdev->notify_base = virtio_pci_cap_addr(pdev, &cap_notify);
  vdev->isr = (uint8_t __iomem *)virtio_pci_cap_addr(pdev, &cap_isr);
  if (has_dev_cfg) {
    vdev->dev_cfg = virtio_pci_cap_addr(pdev, &cap_dev);
  }

  /* Read notify_off_multiplier (it's the 4 bytes after the base virtio_pci_cap)
   */
  {
    uint8_t cap_off =
        (uint8_t)(pci_read_config(pdev->bus, pdev->dev, pdev->func, 0x34) &
                  0xFF);
    while (cap_off != 0) {
      uint32_t hdr0 =
          pci_read_config(pdev->bus, pdev->dev, pdev->func, cap_off);
      uint8_t cap_vndr = hdr0 & 0xFF;
      uint8_t cap_next = (hdr0 >> 8) & 0xFF;
      if (cap_vndr == PCI_CAP_ID_VNDR &&
          ((hdr0 >> 24) & 0xFF) == VIRTIO_PCI_CAP_NOTIFY_CFG) {
        /* notify_off_multiplier is at cap_off + cap_len - 4 (last 4 bytes of
         * cap) */
        uint8_t cap_len = (hdr0 >> 16) & 0xFF;
        vdev->notify_off_multiplier = pci_read_config(
            pdev->bus, pdev->dev, pdev->func, cap_off + cap_len - 4);
        break;
      }
      cap_off = cap_next;
    }
  }

  if (!vdev->common || !vdev->notify_base || !vdev->isr) {
    printk(LOG_ERROR, "virtio_pci: failed to map capabilities\n");
    return -1;
  }

  printk(LOG_INFO,
         "virtio_pci: common=%p notify=%p isr=%p dev_cfg=%p mult=%u\n",
         vdev->common, vdev->notify_base, vdev->isr, vdev->dev_cfg,
         vdev->notify_off_multiplier);

  /* Reset device: write 0 to status */
  virtio_pci_write_status(vdev, 0);

  /* Acknowledge + driver */
  virtio_pci_write_status(vdev,
                          VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

  return 0;
}

uint8_t virtio_pci_read_status(struct virtio_pci_dev *vdev) {
  return vdev->common->device_status;
}

void virtio_pci_write_status(struct virtio_pci_dev *vdev, uint8_t status) {
  vdev->common->device_status = status;
}

uint32_t virtio_pci_read_features(struct virtio_pci_dev *vdev,
                                  uint32_t select) {
  vdev->common->device_feature_select = select;
  return vdev->common->device_feature;
}

void virtio_pci_write_features(struct virtio_pci_dev *vdev, uint32_t select,
                               uint32_t bits) {
  vdev->common->driver_feature_select = select;
  vdev->common->driver_feature = bits;
}

uint8_t virtio_pci_read_isr(struct virtio_pci_dev *vdev) { return *vdev->isr; }

void virtio_pci_notify(struct virtio_pci_dev *vdev, uint16_t notify_off) {
  void __iomem *addr =
      (void __iomem *)((uint8_t __iomem *)vdev->notify_base +
                       notify_off * vdev->notify_off_multiplier);
  *(uint16_t __iomem *)addr = 0; /* write any value to kick */
}

void virtio_pci_read_dev_cfg(struct virtio_pci_dev *vdev, int offset, void *buf,
                             int len) {
  if (!vdev->dev_cfg) {
    __memset(buf, 0, len);
    return;
  }
  uint8_t *dst = (uint8_t *)buf;
  uint8_t __iomem *src = (uint8_t __iomem *)vdev->dev_cfg + offset;
  for (int i = 0; i < len; i++)
    dst[i] = src[i];
}

/* Negotiate features: only accept VIRTIO_F_VERSION_1.
   Returns 0 on success. */
int virtio_pci_negotiate_features(struct virtio_pci_dev *vdev, uint64_t want) {
  uint32_t dev_lo = virtio_pci_read_features(vdev, 0);
  uint32_t dev_hi = virtio_pci_read_features(vdev, 1);
  uint64_t dev_all = ((uint64_t)dev_hi << 32) | dev_lo;
  uint64_t driver = dev_all & want;
  virtio_pci_write_features(vdev, 0, (uint32_t)(driver & 0xFFFFFFFF));
  virtio_pci_write_features(vdev, 1, (uint32_t)(driver >> 32));
  /* Set FEATURES_OK and check device accepts */
  virtio_pci_write_status(vdev, VIRTIO_STATUS_ACKNOWLEDGE |
                                    VIRTIO_STATUS_DRIVER |
                                    VIRTIO_STATUS_FEATURES_OK);
  uint8_t status = virtio_pci_read_status(vdev);
  if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
    printk(LOG_ERROR, "virtio_pci: device rejected features 0x%llx\n",
           (unsigned long long)driver);
    return -1;
  }
  vdev->features = driver;
  printk(LOG_INFO, "virtio_pci: negotiated features 0x%llx\n",
         (unsigned long long)driver);
  return 0;
}

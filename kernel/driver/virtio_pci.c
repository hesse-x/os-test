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

#include <xos/errno.h>

/* Read a virtio_pci_cap from PCI config space cap chain.
   Returns true if found, fills cap struct. */
static bool virtio_pci_find_cap(struct pci_device *pdev, uint8_t cfg_type,
                                uint8_t min_len, struct virtio_pci_cap *out) {
  /* Walk cap chain starting at offset 0x34 (cap pointer in config header) */
  uint8_t cap_off =
      (uint8_t)(pci_read_config(pdev->bus, pdev->dev, pdev->func, 0x34) & 0xFF);
  uint64_t visited = 0;
  unsigned int steps = 0;
  while (cap_off != 0) {
    if (cap_off < 0x40 || cap_off > 0xfc || (cap_off & 3) || ++steps > 48 ||
        (visited & (1ULL << (cap_off >> 2)))) {
      printk(LOG_ERROR, "virtio_pci: malformed capability chain at %#x\n",
             cap_off);
      return false;
    }
    visited |= 1ULL << (cap_off >> 2);
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
      if (this_cfg_type == cfg_type && cap_len >= min_len && bar < 6) {
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
    if (cap_next && (cap_next < 0x40 || cap_next > 0xfc || (cap_next & 3)))
      return false;
    cap_off = cap_next;
  }
  return false;
}

/* Get the kernel virtual address of a capability region (BAR vaddr + offset) */
static void __iomem *virtio_pci_cap_addr(struct pci_device *pdev,
                                         struct virtio_pci_cap *cap) {
  if (cap->bar >= 6 || cap->length == 0)
    return NULL;
  void __iomem *bar_vaddr = pdev->bar[cap->bar].vaddr;
  uint64_t bar_size = pdev->bar[cap->bar].size;
  if (!bar_vaddr || cap->offset > bar_size ||
      cap->length > bar_size - cap->offset)
    return NULL;
  return (void __iomem *)((uint8_t __iomem *)bar_vaddr + cap->offset);
}

int virtio_pci_init(struct virtio_pci_dev *vdev, struct pci_device *pdev) {
  __memset(vdev, 0, sizeof(*vdev));
  vdev->pdev = pdev;

  /* Enable device (maps all BARs) */
  int rc = pci_enable_device(pdev);
  if (rc < 0) {
    printk(LOG_ERROR, "virtio_pci: pci_enable_device failed: %d\n", rc);
    return rc;
  }

  /* Find and map capabilities */
  struct virtio_pci_cap cap_common, cap_notify, cap_isr, cap_dev;
  if (!virtio_pci_find_cap(pdev, VIRTIO_PCI_CAP_COMMON_CFG,
                           sizeof(struct virtio_pci_cap), &cap_common) ||
      cap_common.length < sizeof(struct virtio_pci_common_cfg)) {
    printk(LOG_ERROR, "virtio_pci: common_cfg cap not found\n");
    return -ENODEV;
  }
  if (!virtio_pci_find_cap(pdev, VIRTIO_PCI_CAP_NOTIFY_CFG,
                           sizeof(struct virtio_pci_notify_cap), &cap_notify)) {
    printk(LOG_ERROR, "virtio_pci: notify cap not found\n");
    return -ENODEV;
  }
  if (!virtio_pci_find_cap(pdev, VIRTIO_PCI_CAP_ISR_CFG,
                           sizeof(struct virtio_pci_cap), &cap_isr) ||
      cap_isr.length < 1) {
    printk(LOG_ERROR, "virtio_pci: isr cap not found\n");
    return -ENODEV;
  }
  /* device_cfg is optional for some devices but required for virtio-gpu */
  bool has_dev_cfg = virtio_pci_find_cap(
      pdev, VIRTIO_PCI_CAP_DEVICE_CFG, sizeof(struct virtio_pci_cap), &cap_dev);

  vdev->common = (struct virtio_pci_common_cfg __iomem *)virtio_pci_cap_addr(
      pdev, &cap_common);
  vdev->notify_base = virtio_pci_cap_addr(pdev, &cap_notify);
  vdev->notify_length = cap_notify.length;
  vdev->isr = (uint8_t __iomem *)virtio_pci_cap_addr(pdev, &cap_isr);
  if (has_dev_cfg) {
    vdev->dev_cfg = virtio_pci_cap_addr(pdev, &cap_dev);
    vdev->dev_cfg_length = cap_dev.length;
  }

  /* Read notify_off_multiplier (it's the 4 bytes after the base virtio_pci_cap)
   */
  {
    uint8_t cap_off =
        (uint8_t)(pci_read_config(pdev->bus, pdev->dev, pdev->func, 0x34) &
                  0xFF);
    unsigned int steps = 0;
    while (cap_off != 0 && ++steps <= 48) {
      uint32_t hdr0 =
          pci_read_config(pdev->bus, pdev->dev, pdev->func, cap_off);
      uint8_t cap_vndr = hdr0 & 0xFF;
      uint8_t cap_next = (hdr0 >> 8) & 0xFF;
      if (cap_vndr == PCI_CAP_ID_VNDR &&
          ((hdr0 >> 24) & 0xFF) == VIRTIO_PCI_CAP_NOTIFY_CFG) {
        /* notify_off_multiplier is at cap_off + cap_len - 4 (last 4 bytes of
         * cap) */
        uint8_t cap_len = (hdr0 >> 16) & 0xFF;
        if (cap_len >= sizeof(struct virtio_pci_notify_cap))
          vdev->notify_off_multiplier =
              pci_read_config(pdev->bus, pdev->dev, pdev->func,
                              cap_off + offsetof(struct virtio_pci_notify_cap,
                                                 notify_off_multiplier));
        break;
      }
      cap_off = cap_next;
    }
  }

  if (!vdev->common || !vdev->notify_base || !vdev->isr ||
      !vdev->notify_off_multiplier) {
    printk(LOG_ERROR, "virtio_pci: failed to map capabilities\n");
    return -EFAULT;
  }

  printk(LOG_INFO,
         "virtio_pci: common=%p notify=%p isr=%p dev_cfg=%p mult=%u\n",
         vdev->common, vdev->notify_base, vdev->isr, vdev->dev_cfg,
         vdev->notify_off_multiplier);

  rc = virtio_pci_reset(vdev);
  if (rc)
    return rc;

  /* Acknowledge + driver */
  virtio_pci_write_status(vdev,
                          VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

  return 0;
}

int virtio_pci_reset(struct virtio_pci_dev *vdev) {
  if (!vdev || !vdev->common)
    return -EINVAL;
  virtio_pci_write_status(vdev, 0);
  for (unsigned int i = 0; i < 1000000; i++) {
    if (virtio_pci_read_status(vdev) == 0)
      return 0;
    __asm__ volatile("pause" ::: "memory");
  }
  printk(LOG_ERROR, "virtio_pci: device reset timed out\n");
  return -ETIMEDOUT;
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

int virtio_pci_notify(struct virtio_pci_dev *vdev, uint16_t queue_index,
                      uint16_t notify_off) {
  if (!vdev)
    return -EINVAL;
  uint64_t byte_off = (uint64_t)notify_off * vdev->notify_off_multiplier;
  if (!vdev->notify_base || byte_off > vdev->notify_length ||
      sizeof(uint16_t) > vdev->notify_length - byte_off)
    return -ERANGE;
  void __iomem *addr = (uint8_t __iomem *)vdev->notify_base + byte_off;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  mmio_write16(addr, queue_index);
  return 0;
}

int virtio_pci_read_dev_cfg(struct virtio_pci_dev *vdev, uint32_t offset,
                            void *buf, uint32_t len) {
  if (!vdev || !buf || !vdev->dev_cfg || offset > vdev->dev_cfg_length ||
      len > vdev->dev_cfg_length - offset)
    return -ENODEV;
  uint8_t *dst = (uint8_t *)buf;
  uint8_t __iomem *src = (uint8_t __iomem *)vdev->dev_cfg + offset;
  for (uint32_t i = 0; i < len; i++)
    dst[i] = src[i];
  return 0;
}

/* Negotiate features: only accept VIRTIO_F_VERSION_1.
   Returns 0 on success. */
int virtio_pci_negotiate_features(struct virtio_pci_dev *vdev, uint64_t want) {
  uint32_t dev_lo = virtio_pci_read_features(vdev, 0);
  uint32_t dev_hi = virtio_pci_read_features(vdev, 1);
  uint64_t dev_all = ((uint64_t)dev_hi << 32) | dev_lo;
  uint64_t driver = dev_all & want;
  if ((want & (1ULL << VIRTIO_F_VERSION_1)) &&
      !(driver & (1ULL << VIRTIO_F_VERSION_1))) {
    printk(LOG_ERROR, "virtio_pci: device lacks VERSION_1\n");
    return -EOPNOTSUPP;
  }
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
    return -EIO;
  }
  vdev->features = driver;
  printk(LOG_INFO, "virtio_pci: negotiated features 0x%llx\n",
         (unsigned long long)driver);
  return 0;
}

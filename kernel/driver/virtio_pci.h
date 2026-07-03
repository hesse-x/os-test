#ifndef KERNEL_DRIVER_VIRTIO_PCI_H
#define KERNEL_DRIVER_VIRTIO_PCI_H

#include "kernel/xcore/sparse.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ===== virtio PCI vendor/device IDs ===== */
#define VIRTIO_PCI_VENDOR_ID 0x1AF4
#define VIRTIO_PCI_DEVICE_ID 0x1050 /* transitional device id range base */

/* QEMU virtio-gpu-pci is a non-transitional device:
     vendor_id=0x1AF4, device_id=0x1050 (virtio-gpu non-transitional ID).
   subsystem_id is 0x1100 (QEMU default for all devices) and does NOT identify
   the device type, so we match by vendor+device only (pci_subsystem_id=0).
   Match rule: vendor_id==0x1AF4 && device_id==0x1050. */
#define VIRTIO_SUBSYS_GPU_ID                                                   \
  0 /* unused: subsystem_id cannot distinguish virtio devices */

/* ===== PCI capability IDs (virtio-specific) ===== */
#define PCI_CAP_ID_VNDR 0x09 /* Vendor-specific capability */

/* virtio_pci_cap config types */
#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_ISR_CFG 3
#define VIRTIO_PCI_CAP_DEVICE_CFG 4
#define VIRTIO_PCI_CAP_PCI_CFG 5

/* ===== common config register layout (virtio 1.x spec) ===== */
/* All fields are LE, accessed via MMIO */
struct virtio_pci_common_cfg {
  uint32_t device_feature_select; /* r/w: selects feature bits 0-31 */
  uint32_t device_feature;        /* r: read selected feature bits */
  uint32_t driver_feature_select; /* r/w: selects driver feature bits */
  uint32_t driver_feature;        /* r/w: write selected feature bits */
  uint16_t config_msix_vector;    /* r/w: MSI-X vector for config change */
  uint16_t num_queues;            /* r: total number of virtqueues */
  uint8_t device_status;          /* r/w: device status register */
  uint8_t config_generation;      /* r: config generation counter */
  uint16_t queue_select;          /* r/w: select queue for queue_* ops */
  uint16_t queue_size;            /* r/w: queue size */
  uint16_t queue_msix_vector;     /* r/w: MSI-X vector for queue */
  uint16_t queue_enable;          /* r/w: enable selected queue */
  uint16_t queue_notify_off;      /* r: notify offset for queue */
  uint32_t queue_desc_lo;         /* r/w: desc table addr low */
  uint32_t queue_desc_hi;         /* r/w: desc table addr high */
  uint32_t queue_avail_lo;        /* r/w: avail ring addr low */
  uint32_t queue_avail_hi;        /* r/w: avail ring addr high */
  uint32_t queue_used_lo;         /* r/w: used ring addr high */
  uint32_t queue_used_hi;         /* r/w: used ring addr high */
};

/* ===== device_status bits ===== */
#define VIRTIO_STATUS_ACKNOWLEDGE 0x01
#define VIRTIO_STATUS_DRIVER 0x02
#define VIRTIO_STATUS_DRIVER_OK 0x04
#define VIRTIO_STATUS_FEATURES_OK 0x08
#define VIRTIO_STATUS_DEVICE_NEEDS_RESET 0x40
#define VIRTIO_STATUS_FAILED 0x80

/* ===== ISR status bits ===== */
#define VIRTIO_ISR_QUEUE_INTR 0x01
#define VIRTIO_ISR_CFG_CHANGE 0x02

/* ===== virtio feature bits ===== */
#define VIRTIO_F_VERSION_1 32 /* bit 32 (select=1, bit 0) */

/* ===== virtio_pci_cap struct (in PCI config space cap chain) ===== */
struct virtio_pci_cap {
  uint8_t cap_vndr;   /* 0x09 (PCI_CAP_ID_VNDR) */
  uint8_t cap_next;   /* next cap offset */
  uint8_t cap_len;    /* capability length (including this header) */
  uint8_t cfg_type;   /* VIRTIO_PCI_CAP_* */
  uint8_t bar;        /* BAR index */
  uint8_t padding[3]; /* pad to 8 bytes */
  uint32_t offset;    /* offset within BAR */
  uint32_t length;    /* length of capability region */
};

/* notify cap extra fields (after virtio_pci_cap) */
struct virtio_pci_notify_cap {
  struct virtio_pci_cap cap;
  uint32_t notify_off_multiplier; /* multiplier for queue_notify_off */
};

/* ===== virtio-pci device state ===== */
struct virtio_pci_dev {
  struct pci_device *pdev;

  /* capability regions (mapped via BAR vaddr + offset) */
  struct virtio_pci_common_cfg __iomem *common;
  void __iomem *notify_base;
  uint32_t notify_off_multiplier;
  uint8_t __iomem *isr;
  void __iomem *dev_cfg; /* device-specific config (virtio-gpu config) */

  /* negotiated features */
  uint64_t features;

  /* MSI-X vector base */
  int msix_vector;
};

/* ===== API ===== */
/* Initialize virtio-pci modern transport for given PCI device.
   Returns 0 on success, negative errno on failure. */
int virtio_pci_init(struct virtio_pci_dev *vdev, struct pci_device *pdev);

/* Negotiate features (mask device features with want). Returns 0 on success. */
int virtio_pci_negotiate_features(struct virtio_pci_dev *vdev, uint64_t want);

/* Read/write common config registers */
uint8_t virtio_pci_read_status(struct virtio_pci_dev *vdev);
void virtio_pci_write_status(struct virtio_pci_dev *vdev, uint8_t status);
uint32_t virtio_pci_read_features(struct virtio_pci_dev *vdev, uint32_t select);
void virtio_pci_write_features(struct virtio_pci_dev *vdev, uint32_t select,
                               uint32_t bits);
uint8_t virtio_pci_read_isr(struct virtio_pci_dev *vdev);

/* Notify (kick) a queue: write to notify_base + notify_off * multiplier */
void virtio_pci_notify(struct virtio_pci_dev *vdev, uint16_t notify_off);

/* Config space access for device-specific config */
void virtio_pci_read_dev_cfg(struct virtio_pci_dev *vdev, int offset, void *buf,
                             int len);

#endif /* KERNEL_DRIVER_VIRTIO_PCI_H */

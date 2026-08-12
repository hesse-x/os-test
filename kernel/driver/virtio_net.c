/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/driver/virtio_net.h"

#include "arch/x64/apic.h"
#include "arch/x64/trap.h"
#include "arch/x64/utils.h"
#include "kernel/driver/pci.h"
#include "kernel/driver/virtio_pci.h"
#include "kernel/driver/virtio_ring.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/net_packet.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/workqueue.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <xos/errno.h>
#include <xos/net_ring.h>

#define VIRTIO_NET_F_MAC 5
#define VIRTIO_NET_F_STATUS 16
#define VIRTIO_NET_S_LINK_UP 1
#define VNET_RX_QUEUE 0
#define VNET_TX_QUEUE 1
#define VNET_QUEUE_SIZE 256
#define VNET_WORK_BUDGET 64

struct virtio_net_hdr {
  uint8_t flags;
  uint8_t gso_type;
  uint16_t hdr_len;
  uint16_t gso_size;
  uint16_t csum_start;
  uint16_t csum_offset;
} __attribute__((packed));

_Static_assert(sizeof(struct virtio_net_hdr) == NETPKT_FRAME_OFFSET,
               "virtio net header size");

enum vnet_state { VNET_PROBING = 1, VNET_LIVE, VNET_RESETTING, VNET_DEAD };

struct vnet_slot_ctx {
  struct virtio_net_device *vnet;
  uint16_t slot;
  uint16_t frame_len;
};

struct virtio_net_device {
  struct pci_device *pdev;
  struct virtio_pci_dev vpci;
  struct virtqueue rxq;
  struct virtqueue txq;
  spinlock rx_lock;
  spinlock tx_lock;
  struct net_packet_broker *broker;
  struct workqueue *completion_wq;
  struct workqueue *lifecycle_wq;
  struct work completion_work;
  struct work reset_work;
  struct vnet_slot_ctx rx_ctx[NETPKT_RX_SLOTS];
  struct vnet_slot_ctx tx_ctx[NETPKT_TX_SLOTS];
  uint8_t mac[6];
  uint8_t link_state;
  volatile uint32_t state;
  bool hardware_live;
  bool irq_enabled;
  bool rx_needs_kick;
  uint64_t rx_packets;
  uint64_t rx_bytes;
  uint64_t rx_drops;
  uint64_t rx_bad_used;
  uint64_t rx_bad_len;
  uint64_t rx_broker_full;
  uint64_t tx_packets;
  uint64_t tx_bytes;
  uint64_t tx_errors;
  uint64_t tx_bad_submit;
  uint64_t tx_queue_full;
  uint64_t irqs;
  uint64_t work_runs;
  uint64_t resets;
  uint64_t owner_opens;
  uint64_t owner_disconnects;
};

static bool valid_mac(const uint8_t mac[6]) {
  bool all_zero = true, all_ff = true;
  for (unsigned int i = 0; i < 6; i++) {
    all_zero &= mac[i] == 0;
    all_ff &= mac[i] == 0xff;
  }
  return !(mac[0] & 1) && !all_zero && !all_ff;
}

static int vnet_read_config(struct virtio_net_device *vnet) {
  if (!vnet->vpci.dev_cfg)
    return -ENODEV;
  for (unsigned int retry = 0; retry < 8; retry++) {
    uint8_t before = vnet->vpci.common->config_generation;
    int rc = virtio_pci_read_dev_cfg(&vnet->vpci, 0, vnet->mac, 6);
    uint16_t status = VIRTIO_NET_S_LINK_UP;
    if (!rc && (vnet->vpci.features & (1ULL << VIRTIO_NET_F_STATUS)))
      rc = virtio_pci_read_dev_cfg(&vnet->vpci, 6, &status, sizeof(status));
    uint8_t after = vnet->vpci.common->config_generation;
    if (!rc && before == after) {
      if (!valid_mac(vnet->mac))
        return -EINVAL;
      vnet->link_state =
          (status & VIRTIO_NET_S_LINK_UP) ? NETPKT_LINK_UP : NETPKT_LINK_DOWN;
      return 0;
    }
  }
  return -EAGAIN;
}

static int vnet_setup_queue(struct virtio_net_device *vnet,
                            struct virtqueue *vq, uint16_t index) {
  struct virtio_pci_common_cfg __iomem *common = vnet->vpci.common;
  common->queue_select = index;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  uint16_t offered = common->queue_size;
  uint16_t size = offered < VNET_QUEUE_SIZE ? offered : VNET_QUEUE_SIZE;
  if (!size || (size & (size - 1)))
    return -EINVAL;
  uint16_t notify_off = common->queue_notify_off;
  int rc = vring_create(vq, index, size, notify_off);
  if (rc)
    return rc;
  common->queue_size = size;
  common->queue_msix_vector = 0;
  if (common->queue_msix_vector == 0xffff) {
    rc = -ENOSPC;
    goto fail;
  }
  common->queue_desc_lo = (uint32_t)vq->desc_phys;
  common->queue_desc_hi = (uint32_t)(vq->desc_phys >> 32);
  common->queue_avail_lo = (uint32_t)vq->avail_phys;
  common->queue_avail_hi = (uint32_t)(vq->avail_phys >> 32);
  common->queue_used_lo = (uint32_t)vq->used_phys;
  common->queue_used_hi = (uint32_t)(vq->used_phys >> 32);
  common->queue_enable = 1;
  if (common->queue_enable != 1) {
    rc = -EIO;
    goto fail;
  }
  return 0;
fail:
  vring_destroy(vq);
  return rc;
}

static int vnet_post_rx(struct virtio_net_device *vnet, uint16_t slot) {
  uint64_t addr = net_packet_rx_phys(vnet->broker, slot);
  uint32_t len = NETPKT_FRAME_OFFSET + NETPKT_MAX_FRAME;
  uint16_t flags = VRING_DESC_F_WRITE;
  if (!addr)
    return -EINVAL;
  int head =
      vring_add_buf(&vnet->rxq, &addr, &len, &flags, 1, &vnet->rx_ctx[slot]);
  if (head < 0)
    return -ENOSPC;
  vnet->rx_needs_kick = true;
  return 0;
}

static void vnet_rx_complete(void *opaque, uint32_t used_len) {
  struct vnet_slot_ctx *ctx = opaque;
  if (!ctx || !ctx->vnet)
    return;
  struct virtio_net_device *vnet = ctx->vnet;
  struct virtio_net_hdr *hdr = net_packet_rx_addr(vnet->broker, ctx->slot);
  bool valid = used_len >= NETPKT_FRAME_OFFSET + 14 &&
               used_len <= NETPKT_FRAME_OFFSET + NETPKT_MAX_FRAME && hdr &&
               hdr->flags == 0 && hdr->gso_type == 0;
  if (!valid) {
    vnet->rx_bad_len++;
    vnet->rx_drops++;
    vnet_post_rx(vnet, ctx->slot);
    return;
  }
  uint16_t frame_len = (uint16_t)(used_len - NETPKT_FRAME_OFFSET);
  int rc = net_packet_rx_publish(vnet->broker, ctx->slot, frame_len, 0);
  if (!rc) {
    vnet->rx_packets++;
    vnet->rx_bytes += frame_len;
  } else {
    if (rc == -ENOSPC)
      vnet->rx_broker_full++;
    vnet->rx_drops++;
    vnet_post_rx(vnet, ctx->slot);
  }
}

static void vnet_tx_complete_cb(void *opaque, uint32_t used_len) {
  (void)used_len;
  struct vnet_slot_ctx *ctx = opaque;
  if (!ctx || !ctx->vnet)
    return;
  ctx->vnet->tx_packets++;
  ctx->vnet->tx_bytes += ctx->frame_len;
  net_packet_tx_complete(ctx->vnet->broker, ctx->slot, 0);
}

static void vnet_process_rx_recycle(struct virtio_net_device *vnet) {
  uint16_t slots[VNET_WORK_BUDGET];
  int count =
      net_packet_rx_take_recycled(vnet->broker, slots, VNET_WORK_BUDGET);
  if (count <= 0)
    return;
  uint64_t flags;
  spin_lock_irqsave(&vnet->rx_lock, &flags);
  for (int i = 0; i < count; i++) {
    if (vnet_post_rx(vnet, slots[i]))
      vnet->rx_drops++;
  }
  if (vnet->rx_needs_kick) {
    vring_kick(&vnet->rxq);
    virtio_pci_notify(&vnet->vpci, vnet->rxq.index, vnet->rxq.notify_off);
    vnet->rx_needs_kick = false;
  }
  spin_unlock_irqrestore(&vnet->rx_lock, flags);
}

static void vnet_process_tx(struct virtio_net_device *vnet) {
  struct net_tx_item items[VNET_WORK_BUDGET];
  int count = net_packet_tx_take_batch(vnet->broker, items, VNET_WORK_BUDGET);
  if (count <= 0)
    return;
  uint64_t flags;
  spin_lock_irqsave(&vnet->tx_lock, &flags);
  bool submitted = false;
  for (int i = 0; i < count; i++) {
    uint16_t slot = items[i].slot;
    void *addr = net_packet_tx_addr(vnet->broker, slot);
    uint64_t phys = net_packet_tx_phys(vnet->broker, slot);
    __memset(addr, 0, sizeof(struct virtio_net_hdr));
    uint32_t len = NETPKT_FRAME_OFFSET + items[i].length;
    uint16_t desc_flags = 0;
    vnet->tx_ctx[slot].frame_len = (uint16_t)items[i].length;
    if (vring_add_buf(&vnet->txq, &phys, &len, &desc_flags, 1,
                      &vnet->tx_ctx[slot]) < 0) {
      vnet->tx_queue_full++;
      net_packet_tx_complete(vnet->broker, slot, -EAGAIN);
      continue;
    }
    submitted = true;
  }
  if (submitted) {
    vring_kick(&vnet->txq);
    virtio_pci_notify(&vnet->vpci, vnet->txq.index, vnet->txq.notify_off);
  }
  spin_unlock_irqrestore(&vnet->tx_lock, flags);
}

static void vnet_completion_work(struct work *work) {
  struct virtio_net_device *vnet =
      LIST_ENTRY(work, struct virtio_net_device, completion_work);
  if (__atomic_load_n(&vnet->state, __ATOMIC_ACQUIRE) != VNET_LIVE)
    return;
  vnet->work_runs++;
  uint64_t flags;
  spin_lock_irqsave(&vnet->rx_lock, &flags);
  int rx = vring_poll_used_budget(&vnet->rxq, VNET_WORK_BUDGET);
  if (vnet->rx_needs_kick) {
    vring_kick(&vnet->rxq);
    virtio_pci_notify(&vnet->vpci, vnet->rxq.index, vnet->rxq.notify_off);
    vnet->rx_needs_kick = false;
  }
  bool rx_more = vring_has_used(&vnet->rxq);
  bool rx_broken = vnet->rxq.broken || rx < 0;
  spin_unlock_irqrestore(&vnet->rx_lock, flags);

  spin_lock_irqsave(&vnet->tx_lock, &flags);
  int tx = vring_poll_used_budget(&vnet->txq, VNET_WORK_BUDGET);
  bool tx_more = vring_has_used(&vnet->txq);
  bool tx_broken = vnet->txq.broken || tx < 0;
  spin_unlock_irqrestore(&vnet->tx_lock, flags);
  if (rx_broken || tx_broken) {
    queue_work(vnet->lifecycle_wq, &vnet->reset_work);
    return;
  }
  vnet_process_rx_recycle(vnet);
  vnet_process_tx(vnet);
  if (rx_more || tx_more)
    queue_work(vnet->completion_wq, &vnet->completion_work);
}

static void vnet_isr(trapframe *frame, void *opaque) {
  (void)frame;
  struct virtio_net_device *vnet = opaque;
  if (!vnet || !__atomic_load_n(&vnet->hardware_live, __ATOMIC_ACQUIRE)) {
    lapic_eoi();
    return;
  }
  uint8_t isr = virtio_pci_read_isr(&vnet->vpci);
  vnet->irqs++;
  if (isr & VIRTIO_ISR_CFG_CHANGE) {
    if (!vnet_read_config(vnet))
      net_packet_broker_set_link(vnet->broker, vnet->link_state);
  }
  if (isr & VIRTIO_ISR_QUEUE_INTR)
    queue_work(vnet->completion_wq, &vnet->completion_work);
  if (virtio_pci_read_status(&vnet->vpci) & VIRTIO_STATUS_DEVICE_NEEDS_RESET)
    queue_work(vnet->lifecycle_wq, &vnet->reset_work);
  lapic_eoi();
}

static int vnet_init_queues(struct virtio_net_device *vnet) {
  int rc = vnet_setup_queue(vnet, &vnet->rxq, VNET_RX_QUEUE);
  if (rc)
    return rc;
  rc = vnet_setup_queue(vnet, &vnet->txq, VNET_TX_QUEUE);
  if (rc)
    return rc;
  vnet->rxq.callback = vnet_rx_complete;
  vnet->txq.callback = vnet_tx_complete_cb;
  for (uint16_t i = 0; i < NETPKT_RX_SLOTS; i++) {
    vnet->rx_ctx[i] = (struct vnet_slot_ctx){.vnet = vnet, .slot = i};
    vnet->tx_ctx[i] = (struct vnet_slot_ctx){.vnet = vnet, .slot = i};
  }
  for (uint16_t i = 0; i < vnet->rxq.size; i++) {
    rc = vnet_post_rx(vnet, i);
    if (rc)
      return rc;
  }
  vring_kick(&vnet->rxq);
  return virtio_pci_notify(&vnet->vpci, vnet->rxq.index, vnet->rxq.notify_off);
}

static void vnet_reset_work(struct work *work) {
  struct virtio_net_device *vnet =
      LIST_ENTRY(work, struct virtio_net_device, reset_work);
  uint32_t expected = VNET_LIVE;
  if (!__atomic_compare_exchange_n(&vnet->state, &expected, VNET_RESETTING,
                                   false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return;
  net_packet_broker_stop(vnet->broker, -EIO, false);
  pci_msix_mask_entry(vnet->pdev, 0);
  __atomic_store_n(&vnet->hardware_live, false, __ATOMIC_RELEASE);
  cancel_work_sync(&vnet->completion_work);
  int rc = virtio_pci_reset(&vnet->vpci);
  if (!rc) {
    vring_destroy(&vnet->rxq);
    vring_destroy(&vnet->txq);
    net_packet_broker_rearm(vnet->broker);
    virtio_pci_write_status(&vnet->vpci,
                            VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    uint64_t want = (1ULL << VIRTIO_F_VERSION_1) | (1ULL << VIRTIO_NET_F_MAC) |
                    (1ULL << VIRTIO_NET_F_STATUS);
    rc = virtio_pci_negotiate_features(&vnet->vpci, want);
    if (!rc)
      rc = vnet_init_queues(vnet);
  }
  if (rc) {
    __atomic_store_n(&vnet->state, VNET_DEAD, __ATOMIC_RELEASE);
    net_packet_broker_stop(vnet->broker, rc, true);
    return;
  }
  vnet->resets++;
  vnet->vpci.common->config_msix_vector = 0;
  virtio_pci_write_status(
      &vnet->vpci, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                       VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);
  __atomic_store_n(&vnet->hardware_live, true, __ATOMIC_RELEASE);
  __atomic_store_n(&vnet->state, VNET_LIVE, __ATOMIC_RELEASE);
  pci_msix_unmask_entry(vnet->pdev, 0);
}

static void broker_kick(void *opaque) {
  struct virtio_net_device *vnet = opaque;
  if (vnet && __atomic_load_n(&vnet->state, __ATOMIC_ACQUIRE) == VNET_LIVE)
    queue_work(vnet->completion_wq, &vnet->completion_work);
}
static void broker_owner_lost(void *opaque) {
  struct virtio_net_device *vnet = opaque;
  if (vnet) {
    vnet->owner_disconnects++;
    queue_work(vnet->completion_wq, &vnet->completion_work);
  }
}
static void broker_reset(void *opaque) {
  struct virtio_net_device *vnet = opaque;
  if (vnet)
    queue_work(vnet->lifecycle_wq, &vnet->reset_work);
}
static void broker_stats(void *opaque, struct netpkt_stats *s) {
  struct virtio_net_device *v = opaque;
  s->device_state = __atomic_load_n(&v->state, __ATOMIC_ACQUIRE);
  s->generation = v->broker ? s->generation : 0;
  s->rx_packets = v->rx_packets;
  s->rx_bytes = v->rx_bytes;
  s->rx_drops = v->rx_drops;
  s->rx_bad_used = v->rx_bad_used;
  s->rx_bad_len = v->rx_bad_len;
  s->rx_broker_full = v->rx_broker_full;
  s->tx_packets = v->tx_packets;
  s->tx_bytes = v->tx_bytes;
  s->tx_errors = v->tx_errors;
  s->tx_bad_submit = v->tx_bad_submit;
  s->tx_queue_full = v->tx_queue_full;
  s->irqs = v->irqs;
  s->work_runs = v->work_runs;
  s->resets = v->resets;
  s->owner_opens = v->owner_opens;
  s->owner_disconnects = v->owner_disconnects;
}
static const struct net_packet_ops broker_ops = {
    .kick = broker_kick,
    .owner_lost = broker_owner_lost,
    .request_reset = broker_reset,
    .get_stats = broker_stats,
};

static void virtio_net_remove(struct pci_device *pdev) {
  struct virtio_net_device *vnet = pci_get_driver_private(pdev);
  if (!vnet)
    return;
  __atomic_store_n(&vnet->state, VNET_DEAD, __ATOMIC_RELEASE);
  if (vnet->broker)
    net_packet_broker_stop(vnet->broker, -ENODEV, true);
  __atomic_store_n(&vnet->hardware_live, false, __ATOMIC_RELEASE);
  pci_disable_interrupts(pdev);
  if (vnet->completion_wq) {
    cancel_work_sync(&vnet->completion_work);
    destroy_workqueue(vnet->completion_wq);
  }
  if (vnet->lifecycle_wq) {
    cancel_work_sync(&vnet->reset_work);
    destroy_workqueue(vnet->lifecycle_wq);
  }
  if (vnet->vpci.common && pdev->enabled)
    virtio_pci_reset(&vnet->vpci);
  vring_destroy(&vnet->rxq);
  vring_destroy(&vnet->txq);
  net_packet_broker_destroy(vnet->broker);
  pci_disable_device(pdev);
  pci_set_driver_private(pdev, NULL);
  kfree(vnet);
}

static int virtio_net_probe(struct pci_device *pdev,
                            const struct pci_device_id *id) {
  (void)id;
  int rc;
  const char *stage = "allocation";
  struct virtio_net_device *vnet = kmalloc(sizeof(*vnet));
  if (!vnet)
    return -ENOMEM;
  __memset(vnet, 0, sizeof(*vnet));
  vnet->pdev = pdev;
  vnet->rx_lock = SPINLOCK_INIT;
  vnet->tx_lock = SPINLOCK_INIT;
  vnet->state = VNET_PROBING;
  pci_set_driver_private(pdev, vnet);
  stage = "transport";
  rc = virtio_pci_init(&vnet->vpci, pdev);
  if (rc)
    goto fail;
  stage = "dma-mask";
  rc = pci_set_dma_mask(pdev, 64);
  if (rc)
    goto fail;
  stage = "features";
  uint64_t want = (1ULL << VIRTIO_F_VERSION_1) | (1ULL << VIRTIO_NET_F_MAC) |
                  (1ULL << VIRTIO_NET_F_STATUS);
  rc = virtio_pci_negotiate_features(&vnet->vpci, want);
  if (rc || !(vnet->vpci.features & (1ULL << VIRTIO_NET_F_MAC))) {
    rc = rc ? rc : -EOPNOTSUPP;
    goto fail;
  }
  stage = "config";
  rc = vnet_read_config(vnet);
  if (rc)
    goto fail;
  stage = "msix";
  rc = pci_enable_msix(pdev, 1);
  if (rc <= 0) {
    rc = rc < 0 ? rc : -ENOSPC;
    goto fail;
  }
  vnet->vpci.msix_vector = pdev->msix_vector_base;
  vnet->vpci.common->config_msix_vector = 0;
  if (vnet->vpci.common->config_msix_vector == 0xffff) {
    rc = -ENOSPC;
    goto fail;
  }
  stage = "workqueues";
  vnet->completion_wq = alloc_ordered_workqueue("virtio-net", NULL, NULL);
  vnet->lifecycle_wq = alloc_ordered_workqueue("virtio-net-life", NULL, NULL);
  if (!vnet->completion_wq || !vnet->lifecycle_wq) {
    rc = -ENOMEM;
    goto fail;
  }
  init_work(&vnet->completion_work, vnet_completion_work);
  init_work(&vnet->reset_work, vnet_reset_work);
  stage = "broker";
  rc = net_packet_broker_create(vnet, &broker_ops, vnet->mac,
                                vnet->vpci.features, vnet->link_state,
                                &vnet->broker);
  if (rc)
    goto fail;
  stage = "queues";
  rc = vnet_init_queues(vnet);
  if (rc)
    goto fail;
  stage = "irq";
  rc = pci_request_irq_ctx(pdev, 0, vnet_isr, vnet);
  if (rc)
    goto fail;
  vnet->irq_enabled = true;
  virtio_pci_write_status(
      &vnet->vpci, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                       VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);
  __atomic_store_n(&vnet->hardware_live, true, __ATOMIC_RELEASE);
  __atomic_store_n(&vnet->state, VNET_LIVE, __ATOMIC_RELEASE);
  pci_msix_unmask_entry(pdev, 0);
  stage = "device-node";
  rc = net_packet_broker_register(vnet->broker);
  if (rc)
    goto fail;
  printk(LOG_INFO,
         "virtio_net: live %02x:%02x:%02x:%02x:%02x:%02x link=%u "
         "features=%#llx rxq=%u txq=%u\n",
         vnet->mac[0], vnet->mac[1], vnet->mac[2], vnet->mac[3], vnet->mac[4],
         vnet->mac[5], vnet->link_state,
         (unsigned long long)vnet->vpci.features, vnet->rxq.size,
         vnet->txq.size);
  return 0;
fail:
  printk(LOG_ERROR, "virtio_net: probe failed stage=%s rc=%d\n", stage, rc);
  virtio_net_remove(pdev);
  return rc;
}

static const struct pci_device_id virtio_net_ids[] = {
    {.vendor = VIRTIO_PCI_VENDOR_ID,
     .device = VIRTIO_PCI_DEVICE_NET,
     .subsystem_vendor = PCI_ANY_ID,
     .subsystem_device = PCI_ANY_ID},
    {0},
};
static const struct pci_driver virtio_net_driver = {
    .name = "virtio-net",
    .id_table = virtio_net_ids,
    .probe = virtio_net_probe,
    .remove = virtio_net_remove,
};

int virtio_net_register_driver(void) {
  return pci_register_driver(&virtio_net_driver);
}

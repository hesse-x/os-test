/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/xcore/net_packet.h"

#include "arch/x64/memlayout.h"
#include "arch/x64/utils.h"
#include "kernel/bsd/devtmpfs.h"
#include "kernel/bsd/poll_types.h"
#include "kernel/bsd/types.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/kpi.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/mm_types.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/wait_queue.h"
#include <stddef.h>
#include <xos/errno.h>
#include <xos/mman.h>
#include <xos/net_ring.h>
#include <xos/socket.h>

struct xtask;

enum netpkt_rx_state { RX_DEVICE = 0, RX_USER };
enum netpkt_tx_state { TX_USER_FREE = 0, TX_DEVICE };

struct net_packet_broker {
  refcount_t refs;
  spinlock lock;
  void *device;
  const struct net_packet_ops *ops;
  struct shm *shm;
  struct page **pages;
  struct netpkt_info *info;
  struct netpkt_ring *rx_ready;
  struct netpkt_ring *rx_recycle;
  struct netpkt_ring *tx_submit;
  struct netpkt_ring *tx_complete;
  uint8_t rx_state[NETPKT_RX_SLOTS];
  uint8_t tx_state[NETPKT_TX_SLOTS];
  uint32_t generation;
  bool owner;
  bool started;
  bool stopped;
  bool permanent;
  bool registered;
  int error;
  wait_queue_head wq;
};

static struct net_packet_broker *netpkt0;

static void *broker_page_addr(struct net_packet_broker *broker, uint32_t page) {
  return (__force void *)phys_to_virt(
      (__force phys_addr_t)broker->shm->page_list[page]);
}

static void broker_fill_info(struct net_packet_broker *broker,
                             const uint8_t mac[6], uint64_t features,
                             uint8_t link_state) {
  struct netpkt_info *i = broker->info;
  __memset(i, 0, sizeof(*i));
  i->size = sizeof(*i);
  i->version = NETPKT_ABI_VERSION;
  i->magic = NETPKT_MAGIC;
  i->abi_version = NETPKT_ABI_VERSION;
  i->total_size = NETPKT_TOTAL_SIZE;
  i->generation = broker->generation;
  i->rx_slots = NETPKT_RX_SLOTS;
  i->tx_slots = NETPKT_TX_SLOTS;
  i->slot_size = NETPKT_SLOT_SIZE;
  i->frame_offset = NETPKT_FRAME_OFFSET;
  i->max_frame = NETPKT_MAX_FRAME;
  i->ring_capacity = NETPKT_RING_CAPACITY;
  i->rx_ready_offset = NETPKT_RX_READY_OFFSET;
  i->rx_recycle_offset = NETPKT_RX_RECYCLE_OFFSET;
  i->tx_submit_offset = NETPKT_TX_SUBMIT_OFFSET;
  i->tx_complete_offset = NETPKT_TX_COMPLETE_OFFSET;
  i->rx_data_offset = NETPKT_RX_DATA_OFFSET;
  i->tx_data_offset = NETPKT_TX_DATA_OFFSET;
  i->negotiated_features = features;
  __memcpy(i->mac, mac, sizeof(i->mac));
  i->link_state = link_state;
}

static void broker_reset_shared(struct net_packet_broker *broker,
                                bool reset_ownership) {
  __memset(broker->rx_ready, 0, sizeof(*broker->rx_ready));
  __memset(broker->rx_recycle, 0, sizeof(*broker->rx_recycle));
  __memset(broker->tx_submit, 0, sizeof(*broker->tx_submit));
  __memset(broker->tx_complete, 0, sizeof(*broker->tx_complete));
  if (reset_ownership) {
    for (uint32_t i = 0; i < NETPKT_RX_SLOTS; i++)
      broker->rx_state[i] = RX_DEVICE;
    for (uint32_t i = 0; i < NETPKT_TX_SLOTS; i++)
      broker->tx_state[i] = TX_USER_FREE;
  }
  broker->info->generation = broker->generation;
}

static bool ring_valid(const struct netpkt_ring *ring, uint32_t *producer,
                       uint32_t *consumer) {
  *producer = __atomic_load_n(&ring->producer, __ATOMIC_ACQUIRE);
  *consumer = __atomic_load_n(&ring->consumer, __ATOMIC_RELAXED);
  return (uint32_t)(*producer - *consumer) <= NETPKT_RING_CAPACITY;
}

static int ring_produce(struct netpkt_ring *ring,
                        const struct netpkt_ring_entry *entry) {
  uint32_t producer = __atomic_load_n(&ring->producer, __ATOMIC_RELAXED);
  uint32_t consumer = __atomic_load_n(&ring->consumer, __ATOMIC_ACQUIRE);
  if ((uint32_t)(producer - consumer) >= NETPKT_RING_CAPACITY)
    return -ENOSPC;
  ring->entries[producer & (NETPKT_RING_CAPACITY - 1)] = *entry;
  __atomic_store_n(&ring->producer, producer + 1, __ATOMIC_RELEASE);
  return 0;
}

static void broker_corrupt(struct net_packet_broker *broker) {
  broker->stopped = true;
  broker->error = -EPROTO;
  broker->generation++;
  broker->info->generation = broker->generation;
  __wake_up(&broker->wq, POLLERR);
}

static int netpkt_open(struct xtask *proc, struct file *file) {
  (void)proc;
  struct net_packet_broker *broker = netpkt0;
  if (!broker)
    return -ENODEV;
  uint64_t flags;
  spin_lock_irqsave(&broker->lock, &flags);
  if (broker->owner || broker->permanent) {
    spin_unlock_irqrestore(&broker->lock, flags);
    return broker->owner ? -EBUSY : -ENODEV;
  }
  broker->owner = true;
  broker->started = false;
  broker->stopped = false;
  broker->error = 0;
  broker->generation++;
  broker_reset_shared(broker, false);
  refcount_inc(&broker->refs);
  file->private_data = broker;
  spin_unlock_irqrestore(&broker->lock, flags);
  return 0;
}

static int netpkt_close(struct xtask *proc, struct file *file) {
  (void)proc;
  struct net_packet_broker *broker = file ? file->private_data : NULL;
  if (!broker)
    return 0;
  uint64_t flags;
  spin_lock_irqsave(&broker->lock, &flags);
  broker->owner = false;
  broker->started = false;
  broker->generation++;
  broker_reset_shared(broker, false);
  file->private_data = NULL;
  spin_unlock_irqrestore(&broker->lock, flags);
  if (broker->ops && broker->ops->owner_lost)
    broker->ops->owner_lost(broker->device);
  __wake_up(&broker->wq, POLLHUP);
  if (refcount_dec_and_test(&broker->refs)) {
    shm_put(broker->shm);
    kfree(broker->pages);
    kfree(broker);
  }
  return 0;
}

static int copy_request(struct netpkt_request *request, void *arg) {
  if (copy_from_user(request, arg, sizeof(*request)))
    return -EFAULT;
  if (request->size != sizeof(*request) ||
      request->version != NETPKT_ABI_VERSION || request->flags)
    return -EPROTO;
  return 0;
}

static long netpkt_ioctl(struct xtask *proc, struct file *file, uint32_t cmd,
                         void *arg) {
  (void)proc;
  struct net_packet_broker *broker = file ? file->private_data : NULL;
  if (!broker)
    return -ENODEV;
  if (cmd == NETPKT_GET_INFO) {
    struct netpkt_info info;
    uint64_t flags;
    spin_lock_irqsave(&broker->lock, &flags);
    info = *broker->info;
    spin_unlock_irqrestore(&broker->lock, flags);
    return copy_to_user(arg, &info, sizeof(info)) ? -EFAULT : 0;
  }
  if (cmd == NETPKT_GET_STATS) {
    struct netpkt_stats stats;
    __memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    stats.version = NETPKT_ABI_VERSION;
    stats.generation = broker->generation;
    if (broker->ops && broker->ops->get_stats)
      broker->ops->get_stats(broker->device, &stats);
    return copy_to_user(arg, &stats, sizeof(stats)) ? -EFAULT : 0;
  }
  struct netpkt_request request;
  int rc = copy_request(&request, arg);
  if (rc)
    return rc;
  uint64_t flags;
  spin_lock_irqsave(&broker->lock, &flags);
  if (request.generation != broker->generation) {
    spin_unlock_irqrestore(&broker->lock, flags);
    return -ESTALE;
  }
  if (cmd == NETPKT_START) {
    if (request.ring_mask || broker->permanent) {
      spin_unlock_irqrestore(&broker->lock, flags);
      return broker->permanent ? -ENODEV : -EINVAL;
    }
    broker->stopped = false;
    broker->error = 0;
    broker->started = true;
  } else if (cmd == NETPKT_KICK) {
    if (!broker->started || !request.ring_mask ||
        (request.ring_mask & ~NETPKT_RING_USER_MASK)) {
      spin_unlock_irqrestore(&broker->lock, flags);
      return -EINVAL;
    }
  } else if (cmd != NETPKT_RESET) {
    spin_unlock_irqrestore(&broker->lock, flags);
    return -ENOTTY;
  }
  spin_unlock_irqrestore(&broker->lock, flags);
  if (cmd == NETPKT_RESET) {
#ifdef TEST
    if (broker->ops && broker->ops->request_reset)
      broker->ops->request_reset(broker->device);
    return 0;
#else
    return -EOPNOTSUPP;
#endif
  }
  if (broker->ops && broker->ops->kick)
    broker->ops->kick(broker->device);
  return 0;
}

static void broker_vma_get(void *owner) {
  struct net_packet_broker *broker = owner;
  refcount_inc(&broker->refs);
}
static void broker_vma_put(void *owner) {
  struct net_packet_broker *broker = owner;
  if (refcount_dec_and_test(&broker->refs)) {
    shm_put(broker->shm);
    kfree(broker->pages);
    kfree(broker);
  }
}
static const struct vma_owner_ops broker_vma_ops = {.get = broker_vma_get,
                                                    .put = broker_vma_put};

static int netpkt_mmap(struct file *file,
                       const struct dev_mmap_request *request,
                       struct dev_mmap_backing *backing) {
  struct net_packet_broker *broker = file ? file->private_data : NULL;
  if (!broker || request->offset ||
      request->requested_length != NETPKT_TOTAL_SIZE ||
      request->length != NETPKT_TOTAL_SIZE || !(request->flags & MAP_SHARED) ||
      (request->prot & PROT_EXEC))
    return -EINVAL;
  backing->owner = broker;
  backing->owner_ops = &broker_vma_ops;
  backing->pages = broker->pages;
  backing->page_count = NETPKT_TOTAL_PAGES;
  broker_vma_get(broker);
  return 0;
}

static __poll netpkt_poll(struct xtask *proc, struct file *file, int events) {
  (void)proc;
  (void)events;
  struct net_packet_broker *broker = file ? file->private_data : NULL;
  if (!broker)
    return POLLHUP;
  __poll result = 0;
  uint64_t flags;
  spin_lock_irqsave(&broker->lock, &flags);
  if (broker->permanent)
    result |= POLLHUP;
  if (broker->stopped)
    result |= POLLERR;
  if (broker->rx_ready->producer != broker->rx_ready->consumer ||
      broker->tx_complete->producer != broker->tx_complete->consumer)
    result |= POLLIN;
  if ((uint32_t)(broker->tx_submit->producer - broker->tx_submit->consumer) <
      NETPKT_RING_CAPACITY)
    result |= POLLOUT;
  spin_unlock_irqrestore(&broker->lock, flags);
  return result;
}

static wait_queue_head *netpkt_wait_queue(struct file *file) {
  struct net_packet_broker *broker = file ? file->private_data : NULL;
  return broker ? &broker->wq : NULL;
}

static const struct dev_ops netpkt_ops = {
    .driver_pid = 0,
    .is_block = false,
    .open_file = netpkt_open,
    .close_file = netpkt_close,
    .ioctl_file = netpkt_ioctl,
    .mmap_prepare_file = netpkt_mmap,
    .poll_file = netpkt_poll,
    .wait_queue_file = netpkt_wait_queue,
};

int net_packet_broker_create(void *device, const struct net_packet_ops *ops,
                             const uint8_t mac[6], uint64_t features,
                             uint8_t link_state,
                             struct net_packet_broker **out) {
  if (!device || !ops || !mac || !out || netpkt0)
    return -EINVAL;
  struct net_packet_broker *broker = kmalloc(sizeof(*broker));
  if (!broker)
    return -ENOMEM;
  __memset(broker, 0, sizeof(*broker));
  broker->lock = SPINLOCK_INIT;
  refcount_set(&broker->refs, 1);
  init_wait_queue_head(&broker->wq);
  broker->device = device;
  broker->ops = ops;
  broker->generation = 1;
  broker->shm = shm_create_internal(NETPKT_TOTAL_PAGES);
  broker->pages = kmalloc(sizeof(*broker->pages) * NETPKT_TOTAL_PAGES);
  if (!broker->shm || !broker->pages)
    goto fail;
  for (uint32_t i = 0; i < NETPKT_TOTAL_PAGES; i++)
    broker->pages[i] = &bfc_frames[PHY_TO_PAGE(broker->shm->page_list[i])];
  broker->info = broker_page_addr(broker, 0);
  broker->rx_ready = broker_page_addr(broker, 1);
  broker->rx_recycle = broker_page_addr(broker, 3);
  broker->tx_submit = broker_page_addr(broker, 5);
  broker->tx_complete = broker_page_addr(broker, 7);
  broker_fill_info(broker, mac, features, link_state);
  broker_reset_shared(broker, true);
  *out = broker;
  return 0;
fail:
  if (broker->shm)
    shm_put(broker->shm);
  kfree(broker->pages);
  kfree(broker);
  return -ENOMEM;
}

int net_packet_broker_register(struct net_packet_broker *broker) {
  if (!broker || broker->registered || netpkt0)
    return -EINVAL;
  int rc = devtmpfs_create_device("netpkt0", (struct dev_ops *)&netpkt_ops,
                                  broker, broker->shm);
  if (rc)
    return rc;
  broker->registered = true;
  netpkt0 = broker;
  return 0;
}

void net_packet_broker_rearm(struct net_packet_broker *broker) {
  if (!broker)
    return;
  uint64_t flags;
  spin_lock_irqsave(&broker->lock, &flags);
  broker->started = false;
  broker->stopped = false;
  broker->error = 0;
  broker_reset_shared(broker, true);
  spin_unlock_irqrestore(&broker->lock, flags);
  __wake_up(&broker->wq, POLLERR);
}

void net_packet_broker_destroy(struct net_packet_broker *broker) {
  if (!broker)
    return;
  if (broker->registered) {
    devtmpfs_remove("netpkt0");
    broker->registered = false;
  }
  if (netpkt0 == broker)
    netpkt0 = NULL;
  if (refcount_dec_and_test(&broker->refs)) {
    shm_put(broker->shm);
    kfree(broker->pages);
    kfree(broker);
  }
}

void net_packet_broker_stop(struct net_packet_broker *broker, int error,
                            bool permanent) {
  if (!broker)
    return;
  uint64_t flags;
  spin_lock_irqsave(&broker->lock, &flags);
  broker->stopped = true;
  broker->permanent = permanent;
  broker->error = error;
  broker->generation++;
  broker->info->generation = broker->generation;
  spin_unlock_irqrestore(&broker->lock, flags);
  __wake_up(&broker->wq, permanent ? POLLHUP : POLLERR);
}

void net_packet_broker_set_link(struct net_packet_broker *broker,
                                uint8_t link_state) {
  if (!broker)
    return;
  broker->info->link_state = link_state;
  __wake_up(&broker->wq, POLLIN);
}

uint64_t net_packet_rx_phys(struct net_packet_broker *broker, uint16_t slot) {
  return slot < NETPKT_RX_SLOTS
             ? broker->shm->page_list[NETPKT_CONTROL_PAGES + slot]
             : 0;
}
void *net_packet_rx_addr(struct net_packet_broker *broker, uint16_t slot) {
  uint64_t phys = net_packet_rx_phys(broker, slot);
  return phys ? (__force void *)phys_to_virt((__force phys_addr_t)phys) : NULL;
}
uint64_t net_packet_tx_phys(struct net_packet_broker *broker, uint16_t slot) {
  return slot < NETPKT_TX_SLOTS
             ? broker->shm
                   ->page_list[NETPKT_CONTROL_PAGES + NETPKT_RX_SLOTS + slot]
             : 0;
}
void *net_packet_tx_addr(struct net_packet_broker *broker, uint16_t slot) {
  uint64_t phys = net_packet_tx_phys(broker, slot);
  return phys ? (__force void *)phys_to_virt((__force phys_addr_t)phys) : NULL;
}

int net_packet_rx_publish(struct net_packet_broker *broker, uint16_t slot,
                          uint16_t frame_len, uint32_t flags) {
  if (!broker || slot >= NETPKT_RX_SLOTS || frame_len > NETPKT_MAX_FRAME)
    return -EINVAL;
  uint64_t irq;
  spin_lock_irqsave(&broker->lock, &irq);
  if (broker->rx_state[slot] != RX_DEVICE) {
    spin_unlock_irqrestore(&broker->lock, irq);
    return -EPROTO;
  }
  if (!broker->owner || !broker->started || broker->stopped) {
    spin_unlock_irqrestore(&broker->lock, irq);
    return -ENOTCONN;
  }
  struct netpkt_ring_entry entry = {.generation = broker->generation,
                                    .slot = slot,
                                    .flags = (uint16_t)flags,
                                    .length = frame_len};
  int rc = ring_produce(broker->rx_ready, &entry);
  if (!rc)
    broker->rx_state[slot] = RX_USER;
  spin_unlock_irqrestore(&broker->lock, irq);
  if (!rc)
    __wake_up(&broker->wq, POLLIN);
  return rc;
}

int net_packet_rx_take_recycled(struct net_packet_broker *broker,
                                uint16_t *slots, uint32_t max_slots) {
  if (!broker || !slots)
    return -EINVAL;
  uint64_t irq;
  spin_lock_irqsave(&broker->lock, &irq);
  uint32_t producer, consumer;
  if (!broker->owner) {
    uint32_t count = 0;
    for (uint32_t slot = 0; slot < NETPKT_RX_SLOTS && count < max_slots;
         slot++) {
      if (broker->rx_state[slot] == RX_USER) {
        broker->rx_state[slot] = RX_DEVICE;
        slots[count++] = (uint16_t)slot;
      }
    }
    spin_unlock_irqrestore(&broker->lock, irq);
    return (int)count;
  }
  if (!ring_valid(broker->rx_recycle, &producer, &consumer)) {
    broker_corrupt(broker);
    spin_unlock_irqrestore(&broker->lock, irq);
    return -EPROTO;
  }
  uint32_t count = 0;
  while (consumer != producer && count < max_slots) {
    struct netpkt_ring_entry *entry =
        &broker->rx_recycle->entries[consumer & (NETPKT_RING_CAPACITY - 1)];
    if (entry->generation != broker->generation ||
        entry->slot >= NETPKT_RX_SLOTS || entry->length || entry->flags ||
        entry->status || broker->rx_state[entry->slot] != RX_USER) {
      broker_corrupt(broker);
      spin_unlock_irqrestore(&broker->lock, irq);
      return -EPROTO;
    }
    broker->rx_state[entry->slot] = RX_DEVICE;
    slots[count++] = entry->slot;
    consumer++;
  }
  __atomic_store_n(&broker->rx_recycle->consumer, consumer, __ATOMIC_RELEASE);
  spin_unlock_irqrestore(&broker->lock, irq);
  return (int)count;
}

int net_packet_tx_take_batch(struct net_packet_broker *broker,
                             struct net_tx_item *items, uint32_t max_items) {
  if (!broker || !items)
    return -EINVAL;
  uint64_t irq;
  spin_lock_irqsave(&broker->lock, &irq);
  uint32_t producer, consumer;
  if (!ring_valid(broker->tx_submit, &producer, &consumer)) {
    broker_corrupt(broker);
    spin_unlock_irqrestore(&broker->lock, irq);
    return -EPROTO;
  }
  uint32_t count = 0;
  while (consumer != producer && count < max_items) {
    struct netpkt_ring_entry *entry =
        &broker->tx_submit->entries[consumer & (NETPKT_RING_CAPACITY - 1)];
    if (entry->generation != broker->generation ||
        entry->slot >= NETPKT_TX_SLOTS || entry->length < 14 ||
        entry->length > NETPKT_MAX_FRAME || entry->flags || entry->status ||
        broker->tx_state[entry->slot] != TX_USER_FREE) {
      broker_corrupt(broker);
      spin_unlock_irqrestore(&broker->lock, irq);
      return -EPROTO;
    }
    broker->tx_state[entry->slot] = TX_DEVICE;
    items[count++] =
        (struct net_tx_item){.slot = entry->slot, .length = entry->length};
    consumer++;
  }
  __atomic_store_n(&broker->tx_submit->consumer, consumer, __ATOMIC_RELEASE);
  spin_unlock_irqrestore(&broker->lock, irq);
  return (int)count;
}

void net_packet_tx_complete(struct net_packet_broker *broker, uint16_t slot,
                            int status) {
  if (!broker || slot >= NETPKT_TX_SLOTS)
    return;
  uint64_t irq;
  spin_lock_irqsave(&broker->lock, &irq);
  if (broker->tx_state[slot] != TX_DEVICE) {
    broker_corrupt(broker);
    spin_unlock_irqrestore(&broker->lock, irq);
    return;
  }
  broker->tx_state[slot] = TX_USER_FREE;
  if (broker->owner && broker->started && !broker->stopped) {
    struct netpkt_ring_entry entry = {
        .generation = broker->generation, .slot = slot, .status = status};
    if (ring_produce(broker->tx_complete, &entry))
      broker_corrupt(broker);
  }
  spin_unlock_irqrestore(&broker->lock, irq);
  __wake_up(&broker->wq, POLLIN | POLLOUT);
}

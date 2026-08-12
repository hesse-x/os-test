/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#include "xos_netif.h"

#include <errno.h>
#include <fcntl.h>
#include <lwip/etharp.h>
#include <lwip/ethip6.h>
#include <lwip/pbuf.h>
#include <netif/ethernet.h>
#include <stddef.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int range_ok(uint32_t offset, uint64_t size, uint32_t total) {
  return offset <= total && size <= (uint64_t)total - offset;
}

static int validate_info(const struct netpkt_info *i) {
  uint64_t ring_bytes = sizeof(struct netpkt_ring);
  uint64_t rx_bytes = (uint64_t)i->rx_slots * i->slot_size;
  uint64_t tx_bytes = (uint64_t)i->tx_slots * i->slot_size;
  if (i->size != sizeof(*i) || i->version != NETPKT_ABI_VERSION || i->flags ||
      i->magic != NETPKT_MAGIC || i->abi_version != NETPKT_ABI_VERSION ||
      i->total_size == 0 || i->ring_capacity != NETPKT_RING_CAPACITY ||
      (i->ring_capacity & (i->ring_capacity - 1)) || !i->rx_slots ||
      !i->tx_slots || i->rx_slots > NETPKT_RX_SLOTS ||
      i->tx_slots > NETPKT_TX_SLOTS || i->slot_size == 0 ||
      i->frame_offset > i->slot_size || i->max_frame < 1514 ||
      i->max_frame > i->slot_size - i->frame_offset)
    return -1;
  return range_ok(i->rx_ready_offset, ring_bytes, i->total_size) &&
                 range_ok(i->rx_recycle_offset, ring_bytes, i->total_size) &&
                 range_ok(i->tx_submit_offset, ring_bytes, i->total_size) &&
                 range_ok(i->tx_complete_offset, ring_bytes, i->total_size) &&
                 range_ok(i->rx_data_offset, rx_bytes, i->total_size) &&
                 range_ok(i->tx_data_offset, tx_bytes, i->total_size)
             ? 0
             : -1;
}

static void rx_free(struct pbuf *p) {
  struct xos_rx_pbuf *rx = (struct xos_rx_pbuf *)p;
  struct xos_netif *port = rx->port;
  if (rx->generation == port->info.generation &&
      port->rx_state[rx->slot] == XOS_SLOT_HELD &&
      port->recycle_tail - port->recycle_head < NETPKT_RX_SLOTS) {
    port->recycle[port->recycle_tail++ & (NETPKT_RX_SLOTS - 1)] = rx->slot;
  }
}

static err_t linkoutput(struct netif *netif, struct pbuf *p) {
  struct xos_netif *port = netif->state;
  uint32_t consumer =
      __atomic_load_n(&port->tx_submit->consumer, __ATOMIC_ACQUIRE);
  if (p->tot_len < 14 || p->tot_len > port->info.max_frame ||
      port->tx_submit_prod - consumer >= port->info.ring_capacity) {
    port->stats.tx_drops++;
    return ERR_MEM;
  }
  uint16_t slot;
  for (slot = 0; slot < port->info.tx_slots; slot++)
    if (port->tx_state[slot] == XOS_SLOT_FREE)
      break;
  if (slot == port->info.tx_slots) {
    port->stats.tx_drops++;
    return ERR_MEM;
  }
  uint8_t *frame = port->tx_data + (uint64_t)slot * port->info.slot_size +
                   port->info.frame_offset;
  if (pbuf_copy_partial(p, frame, p->tot_len, 0) != p->tot_len) {
    port->stats.tx_drops++;
    return ERR_BUF;
  }
  struct netpkt_ring_entry entry = {
      .generation = port->info.generation, .slot = slot, .length = p->tot_len};
  port->tx_submit
      ->entries[port->tx_submit_prod & (port->info.ring_capacity - 1)] = entry;
  port->tx_state[slot] = XOS_SLOT_SUBMITTED;
  port->tx_submit_prod++;
  __atomic_store_n(&port->tx_submit->producer, port->tx_submit_prod,
                   __ATOMIC_RELEASE);
  port->stats.tx_packets++;
  port->stats.tx_bytes += p->tot_len;
  if (++port->stats.tx_inflight > port->stats.tx_inflight_high)
    port->stats.tx_inflight_high = port->stats.tx_inflight;
  return ERR_OK;
}

err_t xos_netif_init(struct netif *netif) {
  struct xos_netif *port = netif->state;
  port->netif = netif;
  netif->name[0] = 'x';
  netif->name[1] = 'n';
  netif->hwaddr_len = 6;
  memcpy(netif->hwaddr, port->info.mac, 6);
  netif->mtu = 1500;
  netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
  netif->output = etharp_output;
  netif->linkoutput = linkoutput;
  netif->hostname = "xos";
  return ERR_OK;
}

int xos_netif_open(struct xos_netif *port, const char *path) {
  memset(port, 0, sizeof(*port));
  port->fd = -1;
  int fd = open(path, O_RDWR | O_CLOEXEC);
  if (fd < 0)
    return -1;
  struct netpkt_info info = {.size = sizeof(info),
                             .version = NETPKT_ABI_VERSION};
  if (ioctl(fd, NETPKT_GET_INFO, &info) < 0 || validate_info(&info) < 0) {
    close(fd);
    errno = EPROTO;
    return -1;
  }
  void *mapping =
      mmap(NULL, info.total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapping == MAP_FAILED) {
    close(fd);
    return -1;
  }
  port->fd = fd;
  port->mapping = mapping;
  port->info = info;
  port->rx_ready = (void *)((uint8_t *)mapping + info.rx_ready_offset);
  port->rx_recycle = (void *)((uint8_t *)mapping + info.rx_recycle_offset);
  port->tx_submit = (void *)((uint8_t *)mapping + info.tx_submit_offset);
  port->tx_complete = (void *)((uint8_t *)mapping + info.tx_complete_offset);
  port->rx_data = (uint8_t *)mapping + info.rx_data_offset;
  port->tx_data = (uint8_t *)mapping + info.tx_data_offset;
  port->rx_ready_cons =
      __atomic_load_n(&port->rx_ready->consumer, __ATOMIC_ACQUIRE);
  port->rx_recycle_prod =
      __atomic_load_n(&port->rx_recycle->producer, __ATOMIC_ACQUIRE);
  port->tx_submit_prod =
      __atomic_load_n(&port->tx_submit->producer, __ATOMIC_ACQUIRE);
  port->tx_kicked = port->tx_submit_prod;
  port->tx_complete_cons =
      __atomic_load_n(&port->tx_complete->consumer, __ATOMIC_ACQUIRE);
  struct netpkt_request req = {.size = sizeof(req),
                               .version = NETPKT_ABI_VERSION,
                               .generation = info.generation};
  if (ioctl(fd, NETPKT_START, &req) < 0) {
    xos_netif_close(port);
    return -1;
  }
  return 0;
}

int xos_netif_epoch_valid(struct xos_netif *port) {
  struct netpkt_info now = {.size = sizeof(now), .version = NETPKT_ABI_VERSION};
  if (port->fd < 0 || ioctl(port->fd, NETPKT_GET_INFO, &now) < 0 ||
      validate_info(&now) < 0 || now.generation != port->info.generation)
    return 0;
  port->info.link_state = now.link_state;
  return 1;
}

int xos_netif_poll(struct xos_netif *port, uint32_t budget) {
  uint32_t producer =
      __atomic_load_n(&port->rx_ready->producer, __ATOMIC_ACQUIRE);
  if (producer - port->rx_ready_cons > port->info.ring_capacity) {
    port->stats.rx_bad_ring++;
    errno = EPROTO;
    return -1;
  }
  uint32_t done = 0;
  while (port->rx_ready_cons != producer && done++ < budget) {
    struct netpkt_ring_entry e =
        port->rx_ready
            ->entries[port->rx_ready_cons & (port->info.ring_capacity - 1)];
    if (e.generation != port->info.generation ||
        e.slot >= port->info.rx_slots || e.flags || e.status || e.length < 14 ||
        e.length > port->info.max_frame ||
        port->rx_state[e.slot] != XOS_SLOT_FREE) {
      port->stats.rx_bad_ring++;
      errno = EPROTO;
      return -1;
    }
    struct xos_rx_pbuf *rx = &port->rx_pbuf[e.slot];
    memset(rx, 0, sizeof(*rx));
    rx->custom.custom_free_function = rx_free;
    rx->port = port;
    rx->generation = port->info.generation;
    rx->slot = e.slot;
    void *frame = port->rx_data + (uint64_t)e.slot * port->info.slot_size +
                  port->info.frame_offset;
    struct pbuf *p = pbuf_alloced_custom(PBUF_RAW, (u16_t)e.length, PBUF_REF,
                                         &rx->custom, frame, (u16_t)e.length);
    if (!p) {
      port->stats.rx_drops++;
      break;
    }
    port->rx_state[e.slot] = XOS_SLOT_HELD;
    if (++port->stats.rx_held > port->stats.rx_held_high)
      port->stats.rx_held_high = port->stats.rx_held;
    port->rx_ready_cons++;
    __atomic_store_n(&port->rx_ready->consumer, port->rx_ready_cons,
                     __ATOMIC_RELEASE);
    port->stats.rx_packets++;
    port->stats.rx_bytes += e.length;
    if (port->netif->input(p, port->netif) != ERR_OK) {
      port->stats.rx_drops++;
      pbuf_free(p);
    }
  }
  uint32_t complete_prod =
      __atomic_load_n(&port->tx_complete->producer, __ATOMIC_ACQUIRE);
  if (complete_prod - port->tx_complete_cons > port->info.ring_capacity) {
    port->stats.tx_bad_ring++;
    errno = EPROTO;
    return -1;
  }
  done = 0;
  while (port->tx_complete_cons != complete_prod && done++ < budget) {
    struct netpkt_ring_entry e =
        port->tx_complete
            ->entries[port->tx_complete_cons & (port->info.ring_capacity - 1)];
    if (e.generation != port->info.generation ||
        e.slot >= port->info.tx_slots || e.length || e.flags ||
        port->tx_state[e.slot] != XOS_SLOT_SUBMITTED) {
      port->stats.tx_bad_ring++;
      errno = EPROTO;
      return -1;
    }
    port->tx_state[e.slot] = XOS_SLOT_FREE;
    if (port->stats.tx_inflight)
      port->stats.tx_inflight--;
    if (e.status)
      port->stats.tx_drops++;
    port->tx_complete_cons++;
  }
  __atomic_store_n(&port->tx_complete->consumer, port->tx_complete_cons,
                   __ATOMIC_RELEASE);
  return (int)done;
}

int xos_netif_flush(struct xos_netif *port, uint32_t budget) {
  uint32_t consumer =
      __atomic_load_n(&port->rx_recycle->consumer, __ATOMIC_ACQUIRE);
  uint32_t done = 0;
  while (port->recycle_head != port->recycle_tail && done < budget &&
         port->rx_recycle_prod - consumer < port->info.ring_capacity) {
    uint16_t slot = port->recycle[port->recycle_head++ & (NETPKT_RX_SLOTS - 1)];
    struct netpkt_ring_entry e = {.generation = port->info.generation,
                                  .slot = slot};
    port->rx_recycle
        ->entries[port->rx_recycle_prod & (port->info.ring_capacity - 1)] = e;
    port->rx_recycle_prod++;
    port->rx_state[slot] = XOS_SLOT_FREE;
    if (port->stats.rx_held)
      port->stats.rx_held--;
    done++;
  }
  __atomic_store_n(&port->rx_recycle->producer, port->rx_recycle_prod,
                   __ATOMIC_RELEASE);
  if (!done && port->tx_kicked == port->tx_submit_prod)
    return 0;
  struct netpkt_request req = {.size = sizeof(req),
                               .version = NETPKT_ABI_VERSION,
                               .generation = port->info.generation,
                               .ring_mask = NETPKT_RING_RX_RECYCLE |
                                            NETPKT_RING_TX_SUBMIT};
  int rc = ioctl(port->fd, NETPKT_KICK, &req);
  if (!rc)
    port->tx_kicked = port->tx_submit_prod;
  return rc;
}

void xos_netif_close(struct xos_netif *port) {
  if (port->mapping && port->mapping != MAP_FAILED)
    munmap(port->mapping, port->info.total_size);
  if (port->fd >= 0)
    close(port->fd);
  memset(port, 0, sizeof(*port));
  port->fd = -1;
}

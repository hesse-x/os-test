/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef XOS_NETIF_H
#define XOS_NETIF_H

#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <stdint.h>
#include <xos/net_ring.h>

enum xos_slot_state { XOS_SLOT_FREE, XOS_SLOT_HELD, XOS_SLOT_SUBMITTED };

struct xos_netif;
struct xos_rx_pbuf {
  struct pbuf_custom custom;
  struct xos_netif *port;
  uint32_t generation;
  uint16_t slot;
};

struct xos_netif_stats {
  uint64_t rx_packets, rx_bytes, rx_drops, rx_bad_ring;
  uint64_t tx_packets, tx_bytes, tx_drops, tx_bad_ring;
  uint32_t rx_held, rx_held_high, tx_inflight, tx_inflight_high;
};

struct xos_netif {
  int fd;
  void *mapping;
  struct netpkt_info info;
  struct netpkt_ring *rx_ready, *rx_recycle, *tx_submit, *tx_complete;
  uint8_t *rx_data, *tx_data;
  uint32_t rx_ready_cons, rx_recycle_prod, tx_submit_prod, tx_complete_cons;
  uint8_t rx_state[NETPKT_RX_SLOTS], tx_state[NETPKT_TX_SLOTS];
  struct xos_rx_pbuf rx_pbuf[NETPKT_RX_SLOTS];
  uint16_t recycle[NETPKT_RX_SLOTS];
  uint32_t recycle_head, recycle_tail;
  uint32_t tx_kicked;
  struct netif *netif;
  struct xos_netif_stats stats;
};

int xos_netif_open(struct xos_netif *port, const char *path);
err_t xos_netif_init(struct netif *netif);
int xos_netif_poll(struct xos_netif *port, uint32_t budget);
int xos_netif_flush(struct xos_netif *port, uint32_t budget);
int xos_netif_epoch_valid(struct xos_netif *port);
void xos_netif_close(struct xos_netif *port);

#endif

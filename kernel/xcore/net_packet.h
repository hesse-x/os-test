/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_XCORE_NET_PACKET_H
#define KERNEL_XCORE_NET_PACKET_H

#include <stdbool.h>
#include <stdint.h>

struct net_packet_broker;
struct netpkt_stats;

struct net_packet_ops {
  void (*kick)(void *device);
  void (*owner_lost)(void *device);
  void (*request_reset)(void *device);
  void (*get_stats)(void *device, struct netpkt_stats *stats);
};

struct net_tx_item {
  uint16_t slot;
  uint16_t flags;
  uint32_t length;
};

int net_packet_broker_create(void *device, const struct net_packet_ops *ops,
                             const uint8_t mac[6], uint64_t features,
                             uint8_t link_state,
                             struct net_packet_broker **out);
int net_packet_broker_register(struct net_packet_broker *broker);
void net_packet_broker_rearm(struct net_packet_broker *broker);
void net_packet_broker_destroy(struct net_packet_broker *broker);
void net_packet_broker_stop(struct net_packet_broker *broker, int error,
                            bool permanent);
void net_packet_broker_set_link(struct net_packet_broker *broker,
                                uint8_t link_state);

uint64_t net_packet_rx_phys(struct net_packet_broker *broker, uint16_t slot);
void *net_packet_rx_addr(struct net_packet_broker *broker, uint16_t slot);
uint64_t net_packet_tx_phys(struct net_packet_broker *broker, uint16_t slot);
void *net_packet_tx_addr(struct net_packet_broker *broker, uint16_t slot);

int net_packet_rx_publish(struct net_packet_broker *broker, uint16_t slot,
                          uint16_t frame_len, uint32_t flags);
int net_packet_rx_take_recycled(struct net_packet_broker *broker,
                                uint16_t *slots, uint32_t max_slots);
int net_packet_tx_take_batch(struct net_packet_broker *broker,
                             struct net_tx_item *items, uint32_t max_items);
void net_packet_tx_complete(struct net_packet_broker *broker, uint16_t slot,
                            int status);

#endif

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef XOS_NET_RING_H
#define XOS_NET_RING_H

#include <stddef.h>
#include <stdint.h>
#include <xos/ioctl.h>

#define NETPKT_MAGIC 0x4e504b54u
#define NETPKT_ABI_VERSION 1u
#define NETPKT_RING_CAPACITY 256u
#define NETPKT_SLOT_SIZE 4096u
#define NETPKT_FRAME_OFFSET 10u
#define NETPKT_MAX_FRAME 1514u

#define NETPKT_LINK_UNKNOWN 0u
#define NETPKT_LINK_DOWN 1u
#define NETPKT_LINK_UP 2u

#define NETPKT_RING_RX_READY (1u << 0)
#define NETPKT_RING_RX_RECYCLE (1u << 1)
#define NETPKT_RING_TX_SUBMIT (1u << 2)
#define NETPKT_RING_TX_COMPLETE (1u << 3)
#define NETPKT_RING_USER_MASK (NETPKT_RING_RX_RECYCLE | NETPKT_RING_TX_SUBMIT)

struct netpkt_ring_entry {
  uint32_t generation;
  uint16_t slot;
  uint16_t flags;
  uint32_t length;
  int32_t status;
};

struct netpkt_ring {
  _Alignas(64) uint32_t producer;
  uint8_t producer_pad[60];
  _Alignas(64) uint32_t consumer;
  uint8_t consumer_pad[60];
  struct netpkt_ring_entry entries[NETPKT_RING_CAPACITY];
};

#define NETPKT_RING_BYTES 4224u
#define NETPKT_RING_PAGES 2u
#define NETPKT_INFO_PAGES 1u
#define NETPKT_CONTROL_PAGES (NETPKT_INFO_PAGES + 4u * NETPKT_RING_PAGES)
#define NETPKT_RX_SLOTS 256u
#define NETPKT_TX_SLOTS 256u
#define NETPKT_TOTAL_PAGES                                                     \
  (NETPKT_CONTROL_PAGES + NETPKT_RX_SLOTS + NETPKT_TX_SLOTS)

#define NETPKT_RX_READY_OFFSET (1u * NETPKT_SLOT_SIZE)
#define NETPKT_RX_RECYCLE_OFFSET (3u * NETPKT_SLOT_SIZE)
#define NETPKT_TX_SUBMIT_OFFSET (5u * NETPKT_SLOT_SIZE)
#define NETPKT_TX_COMPLETE_OFFSET (7u * NETPKT_SLOT_SIZE)
#define NETPKT_RX_DATA_OFFSET (NETPKT_CONTROL_PAGES * NETPKT_SLOT_SIZE)
#define NETPKT_TX_DATA_OFFSET                                                  \
  ((NETPKT_CONTROL_PAGES + NETPKT_RX_SLOTS) * NETPKT_SLOT_SIZE)
#define NETPKT_TOTAL_SIZE (NETPKT_TOTAL_PAGES * NETPKT_SLOT_SIZE)

struct netpkt_info {
  uint32_t size;
  uint16_t version;
  uint16_t flags;
  uint32_t magic;
  uint32_t abi_version;
  uint32_t total_size;
  uint32_t generation;
  uint16_t rx_slots;
  uint16_t tx_slots;
  uint32_t slot_size;
  uint32_t frame_offset;
  uint32_t max_frame;
  uint32_t ring_capacity;
  uint32_t rx_ready_offset;
  uint32_t rx_recycle_offset;
  uint32_t tx_submit_offset;
  uint32_t tx_complete_offset;
  uint32_t rx_data_offset;
  uint32_t tx_data_offset;
  uint64_t negotiated_features;
  uint8_t mac[6];
  uint8_t link_state;
  uint8_t reserved0;
  uint8_t reserved[40];
};

struct netpkt_request {
  uint32_t size;
  uint16_t version;
  uint16_t flags;
  uint32_t generation;
  uint32_t ring_mask;
};

struct netpkt_stats {
  uint32_t size;
  uint16_t version;
  uint16_t flags;
  uint32_t generation;
  uint32_t device_state;
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
  uint64_t reserved[8];
};

#define NETPKT_GET_INFO _IOR('N', 0x00, struct netpkt_info)
#define NETPKT_START _IOW('N', 0x01, struct netpkt_request)
#define NETPKT_KICK _IOW('N', 0x02, struct netpkt_request)
#define NETPKT_GET_STATS _IOR('N', 0x03, struct netpkt_stats)
#define NETPKT_RESET _IOW('N', 0x04, struct netpkt_request)

_Static_assert(sizeof(struct netpkt_ring_entry) == 16, "netpkt ring entry ABI");
_Static_assert(sizeof(struct netpkt_ring) == NETPKT_RING_BYTES,
               "netpkt ring ABI");
_Static_assert(sizeof(struct netpkt_info) == 128, "netpkt info ABI");
_Static_assert(sizeof(struct netpkt_request) == 16, "netpkt request ABI");

#endif

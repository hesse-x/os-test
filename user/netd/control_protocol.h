/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef XOS_NETD_CONTROL_PROTOCOL_H
#define XOS_NETD_CONTROL_PROTOCOL_H

#include <stdint.h>

#define NETD_CTL_MAGIC 0x4e455444u
#define NETD_CTL_VERSION 1u

enum netd_ctl_op {
  NETD_CTL_STATUS = 1,
  NETD_CTL_STATS = 2,
  NETD_CTL_PING4 = 3,
  NETD_CTL_UDP_ECHO4 = 4,
  NETD_CTL_DUMP_STATE = 5
};
enum netd_ctl_status {
  NETD_CTL_OK = 0,
  NETD_CTL_BAD_REQUEST = 1,
  NETD_CTL_OFFLINE = 2,
  NETD_CTL_TIMEOUT = 3,
  NETD_CTL_LOCAL_ERROR = 4,
  NETD_CTL_STALE = 5
};

struct netd_ctl_header {
  uint32_t magic;
  uint16_t version;
  uint16_t op;
  uint32_t flags;
  uint32_t request_id;
  uint32_t payload_len;
  uint32_t net_epoch;
};

struct netd_ctl_response {
  struct netd_ctl_header header;
  int32_t status;
  int32_t detail;
};

struct netd_ping_request {
  uint32_t address;
  uint32_t timeout_ms;
};
struct netd_udp_request {
  uint32_t address;
  uint16_t port;
  uint16_t length;
  uint32_t timeout_ms;
  uint8_t payload[512];
};

_Static_assert(sizeof(struct netd_ctl_header) == 24, "netd control ABI");

#endif

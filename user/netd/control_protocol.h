/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef XOS_NETD_CONTROL_PROTOCOL_H
#define XOS_NETD_CONTROL_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define NETD_CTL_MAGIC 0x4e455444u
#define NETD_CTL_VERSION 1u

enum netd_ctl_op {
  NETD_CTL_STATUS = 1,
  NETD_CTL_STATS = 2,
  NETD_CTL_PING4 = 3,
  NETD_CTL_UDP_ECHO4 = 4,
  NETD_CTL_DUMP_STATE = 5,
  NETD_CTL_SNTP4_EXCHANGE = 6
};
enum netd_ctl_status {
  NETD_CTL_OK = 0,
  NETD_CTL_BAD_REQUEST = 1,
  NETD_CTL_OFFLINE = 2,
  NETD_CTL_TIMEOUT = 3,
  NETD_CTL_LOCAL_ERROR = 4,
  NETD_CTL_STALE = 5,
  NETD_CTL_BAD_RESPONSE = 6
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

#define NETD_SNTP_REQUEST_LEN 48u
#define NETD_SNTP_RESPONSE_MAX 512u
struct netd_sntp4_request {
  uint32_t server_addr_be;
  uint32_t timeout_ms;
  uint16_t request_len;
  uint16_t reserved;
  uint8_t request[NETD_SNTP_REQUEST_LEN];
};

struct netd_sntp4_result {
  uint32_t server_addr_be;
  uint16_t server_port_be;
  uint16_t response_len;
  uint64_t send_mono_ns;
  uint64_t recv_mono_ns;
  uint8_t response[NETD_SNTP_RESPONSE_MAX];
};

_Static_assert(sizeof(struct netd_ctl_header) == 24, "netd control ABI");
_Static_assert(sizeof(struct netd_sntp4_request) == 60,
               "netd SNTP request ABI");
_Static_assert(offsetof(struct netd_sntp4_result, response) == 24,
               "netd SNTP response ABI");

#endif

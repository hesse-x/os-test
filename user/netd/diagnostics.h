/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef XOS_NETD_DIAGNOSTICS_H
#define XOS_NETD_DIAGNOSTICS_H

#include "port/xos_netif.h"
#include <lwip/ip_addr.h>
#include <stdint.h>

struct netd_diag_context {
  struct xos_netif *port;
  uint32_t epoch;
};

int netd_ping4(struct netd_diag_context *ctx, uint32_t address,
               uint32_t timeout_ms, uint32_t *rtt_ms);
int netd_udp_echo4(struct netd_diag_context *ctx, uint32_t address,
                   uint16_t port, const void *payload, uint16_t length,
                   uint32_t timeout_ms, uint32_t *rtt_ms);

#endif

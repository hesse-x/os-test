/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef XOS_NETD_CONFIG_H
#define XOS_NETD_CONFIG_H

#define NETD_DEVICE_PATH "/dev/netpkt0"
#define NETD_CONFIG_PATH "/etc/netd.conf"
#define NETD_CONTROL_PATH "/run/netd/control.sock"
#define NETD_READY_PATH "/run/netd/ready"
#define NETD_RX_BUDGET 64u
#define NETD_TX_BUDGET 64u
#define NETD_KICK_BATCH 16u
#define NETD_POLL_MAX_MS 250
#define NETD_DIAG_TIMEOUT_MS 3000u
#define NETD_CONTROL_MAX_PAYLOAD 1024u
#define NETD_CONTROL_CLIENTS 8u
#define NETD_CONFIG_MAX_SIZE 1024u
#define NETD_CONFIG_MAX_LINES 16u

#define NETD_LWIP_VERSION "lwIP-3d896ba0"
#define NETD_LWIP_COMMIT "3d896ba0a37ff3ce73270ca5e230707fe47f60e3"

#endif

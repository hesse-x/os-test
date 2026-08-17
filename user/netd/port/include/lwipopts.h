/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef XOS_NETD_LWIPOPTS_H
#define XOS_NETD_LWIPOPTS_H

#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 0
#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define LWIP_ICMP 1
#define LWIP_RAW 1
#define LWIP_UDP 1
#define LWIP_DHCP 1
#define LWIP_TCP 0
#define LWIP_NETCONN 0
#define LWIP_SOCKET 0
#define LWIP_NETIF_API 0
// DHCP only requests and retains DNS options when the DNS table is enabled.
#define LWIP_DNS 1
#define DNS_MAX_SERVERS 1
#define LWIP_DHCP_MAX_DNS_SERVERS 1
#define IP_FORWARD 0
#define IP_REASSEMBLY 0
#define IP_FRAG 0
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_NETIF_LINK_CALLBACK 1
#define LWIP_NETIF_HOSTNAME 1
#define LWIP_NUM_NETIF_CLIENT_DATA 1
#define LWIP_CHECKSUM_CTRL_PER_NETIF 0
#define CHECKSUM_GEN_IP 1
#define CHECKSUM_GEN_UDP 1
#define CHECKSUM_GEN_ICMP 1
#define CHECKSUM_CHECK_IP 1
#define CHECKSUM_CHECK_UDP 1
#define CHECKSUM_CHECK_ICMP 1

#define MEM_ALIGNMENT 8
#define MEM_SIZE (96 * 1024)
#define MEMP_NUM_PBUF 288
#define MEMP_NUM_RAW_PCB 4
#define MEMP_NUM_UDP_PCB 8
#define MEMP_NUM_SYS_TIMEOUT 16
#define PBUF_POOL_SIZE 32
#define PBUF_POOL_BUFSIZE 1600
#define LWIP_SUPPORT_CUSTOM_PBUF 1
#define ARP_TABLE_SIZE 16
#define ARP_QUEUEING 1
#define ARP_QUEUE_LEN 2
#define LWIP_STATS 1
#define LWIP_STATS_DISPLAY 0

#endif

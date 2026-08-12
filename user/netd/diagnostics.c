/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#include "diagnostics.h"
#include "netd_config.h"

#include <errno.h>
#include <lwip/def.h>
#include <lwip/inet_chksum.h>
#include <lwip/prot/icmp.h>
#include <lwip/raw.h>
#include <lwip/sys.h>
#include <lwip/timeouts.h>
#include <lwip/udp.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>

#define DIAG_MAGIC 0x584e4554u
struct diag_payload {
  uint32_t magic, token, sent_ms;
};

static int address_allowed(uint32_t net_address) {
  uint32_t v = lwip_ntohl(net_address);
  unsigned first = v >> 24;
  return v && v != 0xffffffffu && first != 127 && first < 224;
}

static void pump(struct netd_diag_context *ctx, uint32_t until,
                 volatile int *complete) {
  while (!*complete && (int32_t)(sys_now() - until) < 0) {
    struct pollfd pfd = {.fd = ctx->port->fd, .events = POLLIN};
    int remaining = (int)(until - sys_now());
    if (remaining > NETD_POLL_MAX_MS)
      remaining = NETD_POLL_MAX_MS;
    if (remaining < 0)
      remaining = 0;
    (void)poll(&pfd, 1, remaining);
    if (pfd.revents & (POLLERR | POLLHUP) ||
        xos_netif_poll(ctx->port, NETD_RX_BUDGET) < 0)
      break;
    (void)xos_netif_flush(ctx->port, NETD_TX_BUDGET);
    sys_check_timeouts();
  }
}

struct ping_state {
  volatile int complete;
  uint16_t id, sequence;
  uint32_t token, sent, rtt;
};
static u8_t ping_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p,
                      const ip_addr_t *address) {
  (void)pcb;
  (void)address;
  struct ping_state *s = arg;
  uint8_t bytes[sizeof(struct icmp_echo_hdr) + sizeof(struct diag_payload)];
  /* raw IPv4 callbacks receive a pbuf positioned at the IP header. */
  uint8_t first;
  if (pbuf_copy_partial(p, &first, 1, 0) != 1)
    return 0;
  uint16_t ip_header = (uint16_t)(first & 0x0fu) * 4u;
  if (ip_header < 20 ||
      pbuf_copy_partial(p, bytes, sizeof(bytes), ip_header) != sizeof(bytes))
    return 0;
  struct icmp_echo_hdr *icmp = (void *)bytes;
  struct diag_payload *payload = (void *)(bytes + sizeof(*icmp));
  if (ICMPH_TYPE(icmp) != ICMP_ER || icmp->id != lwip_htons(s->id) ||
      icmp->seqno != lwip_htons(s->sequence) ||
      payload->magic != lwip_htonl(DIAG_MAGIC) ||
      payload->token != lwip_htonl(s->token))
    return 0;
  s->rtt = sys_now() - s->sent;
  s->complete = 1;
  pbuf_free(p);
  return 1;
}

int netd_ping4(struct netd_diag_context *ctx, uint32_t address,
               uint32_t timeout_ms, uint32_t *rtt_ms) {
  if (!ctx || !address_allowed(address) || timeout_ms == 0 ||
      timeout_ms > 30000) {
    errno = EINVAL;
    return -1;
  }
  struct raw_pcb *pcb = raw_new(IP_PROTO_ICMP);
  struct pbuf *p = pbuf_alloc(
      PBUF_IP, sizeof(struct icmp_echo_hdr) + sizeof(struct diag_payload),
      PBUF_RAM);
  if (!pcb || !p) {
    if (pcb)
      raw_remove(pcb);
    if (p)
      pbuf_free(p);
    errno = ENOMEM;
    return -1;
  }
  struct ping_state state = {.id = (uint16_t)(getpid() ^ sys_now()),
                             .sequence = 1,
                             .token = (uint32_t)rand(),
                             .sent = sys_now()};
  struct icmp_echo_hdr *icmp = p->payload;
  ICMPH_TYPE_SET(icmp, ICMP_ECHO);
  ICMPH_CODE_SET(icmp, 0);
  icmp->id = lwip_htons(state.id);
  icmp->seqno = lwip_htons(state.sequence);
  struct diag_payload *payload = (void *)(icmp + 1);
  *payload = (struct diag_payload){
      lwip_htonl(DIAG_MAGIC), lwip_htonl(state.token), lwip_htonl(state.sent)};
  icmp->chksum = 0;
  icmp->chksum = inet_chksum(icmp, p->len);
  raw_recv(pcb, ping_recv, &state);
  ip_addr_t dst;
  ip_addr_set_ip4_u32(&dst, address);
  err_t err = raw_sendto(pcb, p, &dst);
  pbuf_free(p);
  if (err == ERR_OK)
    pump(ctx, state.sent + timeout_ms, &state.complete);
  raw_remove(pcb);
  if (err != ERR_OK) {
    errno = EIO;
    return -1;
  }
  if (!state.complete) {
    errno = ETIMEDOUT;
    return -1;
  }
  if (rtt_ms)
    *rtt_ms = state.rtt;
  return 0;
}

struct udp_state {
  volatile int complete;
  uint32_t address, sent, rtt, token;
  uint16_t port, length;
  const uint8_t *payload;
};
static void udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                        const ip_addr_t *addr, u16_t port) {
  (void)pcb;
  struct udp_state *s = arg;
  if (IP_IS_V4(addr) && ip4_addr_get_u32(ip_2_ip4(addr)) == s->address &&
      port == s->port && p->tot_len == s->length &&
      pbuf_memcmp(p, 0, s->payload, s->length) == 0) {
    s->rtt = sys_now() - s->sent;
    s->complete = 1;
  }
  pbuf_free(p);
}

int netd_udp_echo4(struct netd_diag_context *ctx, uint32_t address,
                   uint16_t port, const void *payload, uint16_t length,
                   uint32_t timeout_ms, uint32_t *rtt_ms) {
  if (!ctx || !address_allowed(address) || !port || !payload || !length ||
      length > 512 || !timeout_ms || timeout_ms > 30000) {
    errno = EINVAL;
    return -1;
  }
  struct udp_pcb *pcb = udp_new();
  struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, length, PBUF_RAM);
  if (!pcb || !p || pbuf_take(p, payload, length) != ERR_OK) {
    if (pcb)
      udp_remove(pcb);
    if (p)
      pbuf_free(p);
    errno = ENOMEM;
    return -1;
  }
  struct udp_state state = {.address = address,
                            .port = port,
                            .length = length,
                            .payload = payload,
                            .sent = sys_now()};
  udp_recv(pcb, udp_recv_cb, &state);
  ip_addr_t dst;
  ip_addr_set_ip4_u32(&dst, address);
  err_t err = udp_sendto(pcb, p, &dst, port);
  pbuf_free(p);
  if (err == ERR_OK)
    pump(ctx, state.sent + timeout_ms, &state.complete);
  udp_remove(pcb);
  if (err != ERR_OK) {
    errno = EIO;
    return -1;
  }
  if (!state.complete) {
    errno = ETIMEDOUT;
    return -1;
  }
  if (rtt_ms)
    *rtt_ms = state.rtt;
  return 0;
}

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#include "sntp_exchange.h"
#include "netd_config.h"

#include <errno.h>
#include <lwip/def.h>
#include <lwip/timeouts.h>
#include <lwip/udp.h>
#include <poll.h>
#include <string.h>
#include <time.h>

struct sntp_transport_state {
  volatile int complete;
  volatile int oversize;
  uint32_t address;
  uint64_t recv_mono_ns;
  uint16_t response_len;
  uint8_t response[NETD_SNTP_RESPONSE_MAX];
};

extern u32_t sys_now(void);

static uint64_t mono_ns(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
    return 0;
  return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static int address_allowed(uint32_t address_be) {
  uint32_t value = lwip_ntohl(address_be);
  unsigned first = value >> 24;
  return value && value != UINT32_MAX && first && first != 127 && first < 224;
}

static void receive(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                    const ip_addr_t *address, u16_t port) {
  (void)pcb;
  struct sntp_transport_state *state = arg;
  if (!IP_IS_V4(address) ||
      ip4_addr_get_u32(ip_2_ip4(address)) != state->address || port != 123) {
    pbuf_free(p);
    return;
  }
  if (p->tot_len > sizeof(state->response)) {
    state->oversize = 1;
    state->complete = 1;
    pbuf_free(p);
    return;
  }
  state->recv_mono_ns = mono_ns();
  state->response_len = p->tot_len;
  pbuf_copy_partial(p, state->response, p->tot_len, 0);
  state->complete = 1;
  pbuf_free(p);
}

int netd_sntp4_exchange(struct netd_diag_context *ctx,
                        const struct netd_sntp4_request *request,
                        struct netd_sntp4_result *result) {
  if (!ctx || !request || !result ||
      !address_allowed(request->server_addr_be) || request->reserved ||
      request->request_len != NETD_SNTP_REQUEST_LEN ||
      request->timeout_ms < 100 || request->timeout_ms > 30000) {
    errno = EINVAL;
    return -1;
  }
  struct udp_pcb *pcb = udp_new();
  struct pbuf *packet =
      pbuf_alloc(PBUF_TRANSPORT, NETD_SNTP_REQUEST_LEN, PBUF_RAM);
  if (!pcb || !packet ||
      pbuf_take(packet, request->request, NETD_SNTP_REQUEST_LEN) != ERR_OK) {
    if (pcb)
      udp_remove(pcb);
    if (packet)
      pbuf_free(packet);
    errno = ENOMEM;
    return -1;
  }
  struct sntp_transport_state state = {.address = request->server_addr_be};
  udp_recv(pcb, receive, &state);
  ip_addr_t destination;
  ip_addr_set_ip4_u32(&destination, request->server_addr_be);
  err_t err = udp_connect(pcb, &destination, 123);
  uint64_t sent = mono_ns();
  if (err == ERR_OK)
    err = udp_send(pcb, packet);
  pbuf_free(packet);
  uint32_t deadline = sys_now() + request->timeout_ms;
  while (err == ERR_OK && !state.complete &&
         (int32_t)(sys_now() - deadline) < 0) {
    struct pollfd pfd = {.fd = ctx->port->fd, .events = POLLIN};
    int remaining = (int)(deadline - sys_now());
    if (remaining > NETD_POLL_MAX_MS)
      remaining = NETD_POLL_MAX_MS;
    if (remaining < 0)
      remaining = 0;
    (void)poll(&pfd, 1, remaining);
    if (pfd.revents & (POLLERR | POLLHUP) ||
        xos_netif_poll(ctx->port, NETD_RX_BUDGET) < 0 ||
        !xos_netif_epoch_valid(ctx->port))
      break;
    (void)xos_netif_flush(ctx->port, NETD_TX_BUDGET);
    sys_check_timeouts();
  }
  udp_remove(pcb);
  if (err != ERR_OK) {
    errno = EIO;
    return -1;
  }
  if (state.oversize) {
    errno = EMSGSIZE;
    return -1;
  }
  if (!state.complete) {
    errno = ETIMEDOUT;
    return -1;
  }
  memset(result, 0, sizeof(*result));
  result->server_addr_be = request->server_addr_be;
  result->server_port_be = lwip_htons(123);
  result->response_len = state.response_len;
  result->send_mono_ns = sent;
  result->recv_mono_ns = state.recv_mono_ns;
  memcpy(result->response, state.response, state.response_len);
  return 0;
}

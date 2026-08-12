/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#include "control_protocol.h"
#include "diagnostics.h"
#include "net_config.h"
#include "netd_config.h"
#include "port/xos_netif.h"
#include "sntp_exchange.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <lwip/dhcp.h>
#include <lwip/dns.h>
#include <lwip/init.h>
#include <lwip/ip4_addr.h>
#include <lwip/timeouts.h>
#include <netif/ethernet.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

enum daemon_state {
  STATE_STARTING,
  STATE_PACKET_READY,
  STATE_CONFIGURING,
  STATE_ONLINE,
  STATE_DEGRADED,
  STATE_RECOVERING
};
static volatile sig_atomic_t stopping;

struct daemon {
  enum daemon_state state;
  struct xos_netif port;
  struct netif netif;
  struct net_config config;
  struct netd_diag_context diagnostics;
  int control_fd;
  uint32_t started_ms;
  uint8_t link_state;
};

static int write_ready(struct daemon *d);

u32_t sys_now(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
    return 0;
  return (u32_t)((uint64_t)ts.tv_sec * 1000u + (uint32_t)ts.tv_nsec / 1000000u);
}

static const char *state_name(enum daemon_state state) {
  static const char *const names[] = {"STARTING", "PACKET_READY", "CONFIGURING",
                                      "ONLINE",   "DEGRADED",     "RECOVERING"};
  return state <= STATE_RECOVERING ? names[state] : "UNKNOWN";
}

static void on_signal(int signo) {
  (void)signo;
  stopping = 1;
}

static void netif_status(struct netif *netif) {
  struct daemon *d = netif->client_data[0];
  if (!ip4_addr_isany_val(*netif_ip4_addr(netif)))
    d->state = STATE_ONLINE;
  else if (d->state != STATE_RECOVERING)
    d->state = STATE_CONFIGURING;
  (void)write_ready(d);
}

static int write_ready(struct daemon *d) {
  char tmp[64], body[160];
  snprintf(tmp, sizeof(tmp), "/run/netd/ready.%d", (int)getpid());
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0)
    return -1;
  int n =
      snprintf(body, sizeof(body), "pid=%d\nepoch=%u\nstate=%s\n",
               (int)getpid(), d->port.info.generation, state_name(d->state));
  int rc = write(fd, body, (size_t)n) == n ? 0 : -1;
  close(fd);
  if (!rc)
    rc = rename(tmp, NETD_READY_PATH);
  if (rc)
    unlink(tmp);
  return rc;
}

static int control_open(void) {
  mkdir("/run/netd", 0755);
  unlink(NETD_CONTROL_PATH);
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  strncpy(addr.sun_path, NETD_CONTROL_PATH, sizeof(addr.sun_path) - 1);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
      chmod(NETD_CONTROL_PATH, 0600) < 0 ||
      listen(fd, NETD_CONTROL_CLIENTS) < 0) {
    close(fd);
    unlink(NETD_CONTROL_PATH);
    return -1;
  }
  return fd;
}

static int read_exact(int fd, void *buffer, size_t length) {
  uint8_t *p = buffer;
  while (length) {
    struct pollfd ready = {.fd = fd, .events = POLLIN};
    if (poll(&ready, 1, 1000) != 1 || !(ready.revents & POLLIN))
      return -1;
    ssize_t n = read(fd, p, length);
    if (n <= 0)
      return -1;
    p += n;
    length -= (size_t)n;
  }
  return 0;
}

static int write_exact(int fd, const void *buffer, size_t length) {
  const uint8_t *p = buffer;
  while (length) {
    struct pollfd ready = {.fd = fd, .events = POLLOUT};
    if (poll(&ready, 1, 1000) != 1 || !(ready.revents & POLLOUT))
      return -1;
    ssize_t n = write(fd, p, length);
    if (n <= 0)
      return -1;
    p += n;
    length -= (size_t)n;
  }
  return 0;
}

static unsigned status_text(struct daemon *d, char *out, unsigned capacity) {
  char addr[16], mask[16], gateway[16], dns[16];
  ip4addr_ntoa_r(netif_ip4_addr(&d->netif), addr, sizeof(addr));
  ip4addr_ntoa_r(netif_ip4_netmask(&d->netif), mask, sizeof(mask));
  ip4addr_ntoa_r(netif_ip4_gw(&d->netif), gateway, sizeof(gateway));
  const ip_addr_t *dns_addr = dns_getserver(0);
  ip4addr_ntoa_r(ip_2_ip4(dns_addr), dns, sizeof(dns));
  int n = snprintf(
      out, capacity,
      "state=%s\npid=%d\nuptime_ms=%u\nlwip=%s\npacket_abi=%u\n"
      "epoch=%u\nlink=%u\nmode=%s\naddress=%s\nnetmask=%s\ngateway=%s\n"
      "dns=%s\nrx_packets=%llu\nrx_drops=%llu\ntx_packets=%llu\ntx_drops=%llu\n"
      "rx_held=%u\ntx_inflight=%u\n",
      state_name(d->state), (int)getpid(), sys_now() - d->started_ms,
      NETD_LWIP_VERSION, NETPKT_ABI_VERSION, d->port.info.generation,
      d->port.info.link_state,
      d->config.mode == NET_CONFIG_DHCP ? "dhcp" : "static", addr, mask,
      gateway, dns, (unsigned long long)d->port.stats.rx_packets,
      (unsigned long long)d->port.stats.rx_drops,
      (unsigned long long)d->port.stats.tx_packets,
      (unsigned long long)d->port.stats.tx_drops, d->port.stats.rx_held,
      d->port.stats.tx_inflight);
  return n < 0 ? 0u : (unsigned)n < capacity ? (unsigned)n : capacity;
}

static void control_client(struct daemon *d) {
  int fd = accept(d->control_fd, NULL, NULL);
  if (fd < 0)
    return;
  struct netd_ctl_header request;
  uint8_t payload[NETD_CONTROL_MAX_PAYLOAD];
  struct netd_ctl_response response;
  memset(&response, 0, sizeof(response));
  if (read_exact(fd, &request, sizeof(request)) < 0 ||
      request.magic != NETD_CTL_MAGIC || request.version != NETD_CTL_VERSION ||
      request.flags || request.payload_len > sizeof(payload) ||
      read_exact(fd, payload, request.payload_len) < 0) {
    response.status = NETD_CTL_BAD_REQUEST;
    request = (struct netd_ctl_header){.magic = NETD_CTL_MAGIC,
                                       .version = NETD_CTL_VERSION};
  }
  response.header = request;
  response.header.net_epoch = d->port.info.generation;
  char text[1024];
  unsigned text_length = 0;
  if (!response.status && request.net_epoch &&
      request.net_epoch != d->port.info.generation)
    response.status = NETD_CTL_STALE;
  else if (!response.status &&
           (request.op == NETD_CTL_STATUS || request.op == NETD_CTL_STATS ||
            request.op == NETD_CTL_DUMP_STATE)) {
    if (request.payload_len)
      response.status = NETD_CTL_BAD_REQUEST;
    else
      text_length = status_text(d, text, sizeof(text));
  } else if (!response.status && request.op == NETD_CTL_PING4) {
    if (request.payload_len != sizeof(struct netd_ping_request))
      response.status = NETD_CTL_BAD_REQUEST;
    else {
      struct netd_ping_request *r = (void *)payload;
      uint32_t rtt;
      if (d->state != STATE_ONLINE)
        response.status = NETD_CTL_OFFLINE;
      else if (netd_ping4(&d->diagnostics, r->address, r->timeout_ms, &rtt) < 0)
        response.status =
            errno == ETIMEDOUT ? NETD_CTL_TIMEOUT : NETD_CTL_LOCAL_ERROR;
      else {
        response.detail = (int32_t)rtt;
        text_length =
            (unsigned)snprintf(text, sizeof(text), "reply rtt_ms=%u\n", rtt);
      }
    }
  } else if (!response.status && request.op == NETD_CTL_UDP_ECHO4) {
    if (request.payload_len < offsetof(struct netd_udp_request, payload))
      response.status = NETD_CTL_BAD_REQUEST;
    else {
      struct netd_udp_request *r = (void *)payload;
      if (r->length > sizeof(r->payload) ||
          request.payload_len !=
              offsetof(struct netd_udp_request, payload) + r->length)
        response.status = NETD_CTL_BAD_REQUEST;
      else if (d->state != STATE_ONLINE)
        response.status = NETD_CTL_OFFLINE;
      else {
        uint32_t rtt;
        if (netd_udp_echo4(&d->diagnostics, r->address, r->port, r->payload,
                           r->length, r->timeout_ms, &rtt) < 0)
          response.status =
              errno == ETIMEDOUT ? NETD_CTL_TIMEOUT : NETD_CTL_LOCAL_ERROR;
        else {
          response.detail = (int32_t)rtt;
          text_length =
              (unsigned)snprintf(text, sizeof(text), "echo rtt_ms=%u\n", rtt);
        }
      }
    }
  } else if (!response.status && request.op == NETD_CTL_SNTP4_EXCHANGE) {
    if (request.payload_len != sizeof(struct netd_sntp4_request))
      response.status = NETD_CTL_BAD_REQUEST;
    else if (d->state != STATE_ONLINE)
      response.status = NETD_CTL_OFFLINE;
    else {
      struct netd_sntp4_result result;
      if (netd_sntp4_exchange(&d->diagnostics, (const void *)payload, &result) <
          0) {
        response.status = errno == ETIMEDOUT  ? NETD_CTL_TIMEOUT
                          : errno == EMSGSIZE ? NETD_CTL_BAD_RESPONSE
                                              : NETD_CTL_LOCAL_ERROR;
      } else {
        size_t result_length =
            offsetof(struct netd_sntp4_result, response) + result.response_len;
        memcpy(payload, &result, result_length);
        text_length = (unsigned)result_length;
      }
    }
  } else if (!response.status)
    response.status = NETD_CTL_BAD_REQUEST;
  response.header.payload_len = text_length;
  (void)write_exact(fd, &response, sizeof(response));
  if (text_length)
    (void)write_exact(
        fd, request.op == NETD_CTL_SNTP4_EXCHANGE ? payload : (void *)text,
        text_length);
  close(fd);
}

static int configure_address(struct daemon *d) {
  if (d->config.mode == NET_CONFIG_DHCP)
    return dhcp_start(&d->netif) == ERR_OK ? 0 : -1;
  ip4_addr_t address = {.addr = d->config.address};
  ip4_addr_t mask = {.addr = d->config.netmask};
  ip4_addr_t gateway = {.addr = d->config.gateway};
  ip_addr_t dns;
  ip_addr_set_ip4_u32(&dns, d->config.dns);
  netif_set_addr(&d->netif, &address, &mask, &gateway);
  dns_setserver(0, &dns);
  d->state = STATE_ONLINE;
  return 0;
}

int main(void) {
  struct daemon d;
  memset(&d, 0, sizeof(d));
  d.port.fd = -1;
  d.control_fd = -1;
  d.state = STATE_STARTING;
  d.started_ms = sys_now();
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  signal(SIGPIPE, SIG_IGN);
  char error[128];
  if (net_config_load(NETD_CONFIG_PATH, &d.config, error, sizeof(error)) < 0) {
    fprintf(stderr, "netd: config: %s\n", error);
    return 2;
  }
  lwip_init();
  if (xos_netif_open(&d.port, NETD_DEVICE_PATH) < 0) {
    fprintf(stderr, "netd: packet open failed: %s\n", strerror(errno));
    return 3;
  }
  d.state = STATE_PACKET_READY;
  ip4_addr_t zero = {0};
  if (!netif_add(&d.netif, &zero, &zero, &zero, &d.port, xos_netif_init,
                 ethernet_input)) {
    xos_netif_close(&d.port);
    return 4;
  }
  d.netif.client_data[0] = &d;
  netif_set_status_callback(&d.netif, netif_status);
  netif_set_default(&d.netif);
  netif_set_up(&d.netif);
  if (d.port.info.link_state != NETPKT_LINK_DOWN)
    netif_set_link_up(&d.netif);
  d.state = STATE_CONFIGURING;
  if (configure_address(&d) < 0)
    d.state = STATE_DEGRADED;
  d.diagnostics = (struct netd_diag_context){.port = &d.port,
                                             .epoch = d.port.info.generation};
  d.link_state = d.port.info.link_state;
  d.control_fd = control_open();
  if (d.control_fd < 0) {
    xos_netif_close(&d.port);
    return 5;
  }
  (void)write_ready(&d);
  printf("netd: %s epoch=%u %s\n", NETD_LWIP_VERSION, d.port.info.generation,
         state_name(d.state));
  while (!stopping) {
    struct pollfd fds[2] = {{.fd = d.port.fd, .events = POLLIN},
                            {.fd = d.control_fd, .events = POLLIN}};
    int rc = poll(fds, 2, NETD_POLL_MAX_MS);
    if (rc < 0 && errno != EINTR)
      break;
    if (fds[0].revents & (POLLERR | POLLHUP) ||
        xos_netif_poll(&d.port, NETD_RX_BUDGET) < 0) {
      d.state = STATE_RECOVERING;
      break;
    }
    (void)xos_netif_flush(&d.port, NETD_TX_BUDGET);
    sys_check_timeouts();
    if (!xos_netif_epoch_valid(&d.port)) {
      d.state = STATE_RECOVERING;
      break;
    }
    if (d.port.info.link_state != d.link_state) {
      d.link_state = d.port.info.link_state;
      if (d.link_state == NETPKT_LINK_DOWN) {
        if (d.config.mode == NET_CONFIG_DHCP)
          dhcp_stop(&d.netif);
        etharp_cleanup_netif(&d.netif);
        ip4_addr_t zero_address = {0};
        netif_set_addr(&d.netif, &zero_address, &zero_address, &zero_address);
        netif_set_link_down(&d.netif);
        d.state = STATE_DEGRADED;
      } else {
        netif_set_link_up(&d.netif);
        d.state = STATE_CONFIGURING;
        if (configure_address(&d) < 0)
          d.state = STATE_DEGRADED;
      }
      (void)write_ready(&d);
    }
    if (fds[1].revents & POLLIN)
      control_client(&d);
  }
  unlink(NETD_READY_PATH);
  unlink(NETD_CONTROL_PATH);
  if (d.config.mode == NET_CONFIG_DHCP)
    dhcp_stop(&d.netif);
  netif_set_down(&d.netif);
  netif_remove(&d.netif);
  close(d.control_fd);
  xos_netif_close(&d.port);
  return stopping ? 0 : 6;
}

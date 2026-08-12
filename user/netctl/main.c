/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#include "../netd/control_protocol.h"
#include "../netd/netd_config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static uint32_t host_to_net32(uint32_t value) {
  return __builtin_bswap32(value);
}

static int write_exact(int fd, const void *buffer, size_t length) {
  const char *p = buffer;
  while (length) {
    ssize_t n = write(fd, p, length);
    if (n <= 0)
      return -1;
    p += n;
    length -= (size_t)n;
  }
  return 0;
}
static int read_exact(int fd, void *buffer, size_t length) {
  char *p = buffer;
  while (length) {
    ssize_t n = read(fd, p, length);
    if (n <= 0)
      return -1;
    p += n;
    length -= (size_t)n;
  }
  return 0;
}

static int parse_address(const char *text, uint32_t *out) {
  uint32_t result = 0;
  for (unsigned part = 0; part < 4; part++) {
    if (*text < '0' || *text > '9')
      return -1;
    unsigned octet = 0, digits = 0;
    do {
      octet = octet * 10u + (unsigned)(*text++ - '0');
      digits++;
    } while (*text >= '0' && *text <= '9' && digits < 3);
    if (octet > 255 || (*text >= '0' && *text <= '9'))
      return -1;
    result = (result << 8) | octet;
    if (part != 3 && *text++ != '.')
      return -1;
  }
  if (*text)
    return -1;
  *out = host_to_net32(result);
  return 0;
}

static int transact(struct netd_ctl_header *header, const void *payload) {
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return 4;
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  strncpy(addr.sun_path, NETD_CONTROL_PATH, sizeof(addr.sun_path) - 1);
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    fprintf(stderr, "netctl: netd is unavailable\n");
    close(fd);
    return 4;
  }
  if (write_exact(fd, header, sizeof(*header)) < 0 ||
      (header->payload_len &&
       write_exact(fd, payload, header->payload_len) < 0)) {
    close(fd);
    return 5;
  }
  struct netd_ctl_response response;
  if (read_exact(fd, &response, sizeof(response)) < 0 ||
      response.header.magic != NETD_CTL_MAGIC ||
      response.header.version != NETD_CTL_VERSION ||
      response.header.payload_len > NETD_CONTROL_MAX_PAYLOAD) {
    close(fd);
    return 5;
  }
  char text[NETD_CONTROL_MAX_PAYLOAD + 1];
  if (read_exact(fd, text, response.header.payload_len) < 0) {
    close(fd);
    return 5;
  }
  text[response.header.payload_len] = '\0';
  if (*text)
    fputs(text, response.status ? stderr : stdout);
  close(fd);
  if (response.status == NETD_CTL_OK)
    return 0;
  if (response.status == NETD_CTL_OFFLINE)
    return 2;
  if (response.status == NETD_CTL_TIMEOUT)
    return 3;
  return 5;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: netctl status|stats|dump-state|ping IP|udp-echo IP "
                    "PORT [payload]\n");
    return 1;
  }
  struct netd_ctl_header h = {.magic = NETD_CTL_MAGIC,
                              .version = NETD_CTL_VERSION,
                              .request_id = (uint32_t)getpid()};
  if (!strcmp(argv[1], "status"))
    h.op = NETD_CTL_STATUS;
  else if (!strcmp(argv[1], "stats"))
    h.op = NETD_CTL_STATS;
  else if (!strcmp(argv[1], "dump-state"))
    h.op = NETD_CTL_DUMP_STATE;
  else if (!strcmp(argv[1], "ping") && argc == 3) {
    struct netd_ping_request r = {.timeout_ms = NETD_DIAG_TIMEOUT_MS};
    if (parse_address(argv[2], &r.address) < 0)
      return 1;
    h.op = NETD_CTL_PING4;
    h.payload_len = sizeof(r);
    return transact(&h, &r);
  } else if (!strcmp(argv[1], "udp-echo") && (argc == 4 || argc == 5)) {
    struct netd_udp_request r = {.timeout_ms = NETD_DIAG_TIMEOUT_MS};
    long port = strtol(argv[3], NULL, 10);
    const char *payload = argc == 5 ? argv[4] : "xos-netd-echo";
    size_t length = strlen(payload);
    if (parse_address(argv[2], &r.address) < 0 || port < 1 || port > 65535 ||
        !length || length > sizeof(r.payload))
      return 1;
    r.port = (uint16_t)port;
    r.length = (uint16_t)length;
    memcpy(r.payload, payload, length);
    h.op = NETD_CTL_UDP_ECHO4;
    h.payload_len =
        (uint32_t)(offsetof(struct netd_udp_request, payload) + length);
    return transact(&h, &r);
  } else
    return 1;
  return transact(&h, NULL);
}

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#include "net_config.h"
#include "netd_config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static uint32_t host_to_net32(uint32_t value) {
  return __builtin_bswap32(value);
}

static int fail(char *error, unsigned size, const char *message) {
  if (size) {
    strncpy(error, message, size - 1);
    error[size - 1] = '\0';
  }
  errno = EINVAL;
  return -1;
}

static int parse_ip(const char *s, uint32_t *value) {
  uint32_t result = 0;
  for (unsigned part = 0; part < 4; part++) {
    if (*s < '0' || *s > '9')
      return -1;
    unsigned octet = 0, digits = 0;
    do {
      octet = octet * 10u + (unsigned)(*s++ - '0');
      digits++;
    } while (*s >= '0' && *s <= '9' && digits < 3);
    if (octet > 255 || (*s >= '0' && *s <= '9'))
      return -1;
    result = (result << 8) | octet;
    if (part != 3) {
      if (*s++ != '.')
        return -1;
    }
  }
  if (*s)
    return -1;
  *value = host_to_net32(result);
  return 0;
}

static int usable_unicast(uint32_t net_value) {
  uint32_t v = host_to_net32(net_value);
  unsigned first = v >> 24;
  return v != 0 && v != 0xffffffffu && first != 127 && first < 224;
}

static int valid_mask(uint32_t net_value) {
  uint32_t m = host_to_net32(net_value);
  uint32_t inverse = ~m;
  return m != 0 && m != 0xffffffffu && (inverse & (inverse + 1u)) == 0;
}

int net_config_parse(const void *data, unsigned size, struct net_config *out,
                     char *error, unsigned error_size) {
  char text[NETD_CONFIG_MAX_SIZE + 1];
  unsigned seen = 0, lines = 0;
  struct net_config candidate = {.mode = NET_CONFIG_DHCP};
  if (!data || !out || size > NETD_CONFIG_MAX_SIZE || memchr(data, 0, size))
    return fail(error, error_size, "invalid file size or embedded NUL");
  memcpy(text, data, size);
  text[size] = '\0';

  char *save = NULL;
  for (char *line = strtok_r(text, "\n", &save); line;
       line = strtok_r(NULL, "\n", &save)) {
    if (++lines > NETD_CONFIG_MAX_LINES)
      return fail(error, error_size, "too many lines");
    if (*line == '\0' || *line == '#')
      continue;
    char *eq = strchr(line, '=');
    if (!eq || eq == line || strchr(eq + 1, '='))
      return fail(error, error_size, "invalid assignment");
    *eq++ = '\0';
    for (char *p = line; *p; p++)
      if (*p < 'a' || *p > 'z')
        return fail(error, error_size, "invalid key");
    unsigned bit;
    if (!strcmp(line, "mode")) {
      bit = 1u;
      if (!strcmp(eq, "dhcp"))
        candidate.mode = NET_CONFIG_DHCP;
      else if (!strcmp(eq, "static"))
        candidate.mode = NET_CONFIG_STATIC;
      else
        return fail(error, error_size, "invalid mode");
    } else if (!strcmp(line, "address"))
      bit = 2u;
    else if (!strcmp(line, "netmask"))
      bit = 4u;
    else if (!strcmp(line, "gateway"))
      bit = 8u;
    else if (!strcmp(line, "dns"))
      bit = 16u;
    else
      return fail(error, error_size, "unknown key");
    if (seen & bit)
      return fail(error, error_size, "duplicate key");
    seen |= bit;
    uint32_t *dst = bit == 2    ? &candidate.address
                    : bit == 4  ? &candidate.netmask
                    : bit == 8  ? &candidate.gateway
                    : bit == 16 ? &candidate.dns
                                : NULL;
    if (dst && parse_ip(eq, dst) < 0)
      return fail(error, error_size, "invalid IPv4 address");
  }
  if (!(seen & 1u))
    return fail(error, error_size, "missing mode");
  if (candidate.mode == NET_CONFIG_DHCP) {
    if (seen != 1u)
      return fail(error, error_size,
                  "DHCP and static fields are mutually exclusive");
  } else {
    if ((seen & 31u) != 31u || !usable_unicast(candidate.address) ||
        !valid_mask(candidate.netmask) || !usable_unicast(candidate.gateway) ||
        !usable_unicast(candidate.dns))
      return fail(error, error_size,
                  "incomplete or invalid static configuration");
    if ((candidate.address & candidate.netmask) !=
        (candidate.gateway & candidate.netmask))
      return fail(error, error_size, "gateway is not on-link");
    uint32_t host = host_to_net32(candidate.address);
    uint32_t mask = host_to_net32(candidate.netmask);
    if ((host & ~mask) == 0 || (host & ~mask) == ~mask)
      return fail(error, error_size, "address is subnet network or broadcast");
  }
  *out = candidate;
  return 0;
}

int net_config_load(const char *path, struct net_config *out, char *error,
                    unsigned error_size) {
  char data[NETD_CONFIG_MAX_SIZE + 1];
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENOENT) {
      *out = (struct net_config){.mode = NET_CONFIG_DHCP};
      return 0;
    }
    return -1;
  }
  ssize_t n = read(fd, data, sizeof(data));
  int saved = errno;
  close(fd);
  errno = saved;
  if (n < 0 || n > NETD_CONFIG_MAX_SIZE)
    return fail(error, error_size, "configuration is too large");
  return net_config_parse(data, (unsigned)n, out, error, error_size);
}

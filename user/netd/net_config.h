/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef XOS_NET_CONFIG_H
#define XOS_NET_CONFIG_H

#include <stdint.h>

enum net_config_mode { NET_CONFIG_DHCP, NET_CONFIG_STATIC };

struct net_config {
  enum net_config_mode mode;
  uint32_t address;
  uint32_t netmask;
  uint32_t gateway;
  uint32_t dns;
};

int net_config_load(const char *path, struct net_config *out, char *error,
                    unsigned error_size);
int net_config_parse(const void *data, unsigned size, struct net_config *out,
                     char *error, unsigned error_size);

#endif

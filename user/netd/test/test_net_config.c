/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#include "../net_config.h"

#include <assert.h>
#include <string.h>

static void rejects(const char *text) {
  struct net_config config = {.address = 0xdeadbeef};
  struct net_config before = config;
  char error[128];
  assert(net_config_parse(text, (unsigned)strlen(text), &config, error,
                          sizeof(error)) < 0);
  assert(memcmp(&config, &before, sizeof(config)) == 0);
}

int main(void) {
  struct net_config config;
  char error[128];
  const char dhcp[] = "mode=dhcp\n";
  assert(net_config_parse(dhcp, sizeof(dhcp) - 1, &config, error,
                          sizeof(error)) == 0);
  assert(config.mode == NET_CONFIG_DHCP);

  const char good[] = "mode=static\naddress=192.0.2.10\n"
                      "netmask=255.255.255.0\ngateway=192.0.2.1\n"
                      "dns=192.0.2.53\n";
  assert(net_config_parse(good, sizeof(good) - 1, &config, error,
                          sizeof(error)) == 0);
  assert(config.mode == NET_CONFIG_STATIC);

  rejects("mode=dhcp\naddress=192.0.2.10\n");
  rejects("mode=static\naddress=192.0.2.10\nnetmask=255.0.255.0\n"
          "gateway=192.0.2.1\ndns=192.0.2.53\n");
  rejects("mode=static\naddress=192.0.2.0\nnetmask=255.255.255.0\n"
          "gateway=192.0.2.1\ndns=192.0.2.53\n");
  rejects("mode=static\naddress=192.0.2.10\nnetmask=255.255.255.0\n"
          "gateway=198.51.100.1\ndns=192.0.2.53\n");
  rejects("mode=dhcp\nmode=dhcp\n");
  rejects("unknown=value\n");
  rejects("mode=static\naddress=999.0.2.10\nnetmask=255.255.255.0\n"
          "gateway=192.0.2.1\ndns=192.0.2.53\n");
  return 0;
}

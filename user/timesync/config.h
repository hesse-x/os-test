/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef XOS_TIMESYNC_CONFIG_H
#define XOS_TIMESYNC_CONFIG_H

#include "sntp.h"
#include <stddef.h>
#include <stdint.h>

#define TIMESYNC_CONFIG_PATH "/etc/timesync.conf"
#define TIMESYNC_MAX_SAMPLES 8u

struct timesync_config {
  uint32_t server_addr_be;
  char server[16];
  uint32_t request_timeout_ms;
  uint32_t total_budget_ms;
  unsigned sample_count;
  struct sntp_policy policy;
};

int timesync_config_load(const char *path, struct timesync_config *config,
                         char *error, size_t error_size);

#endif

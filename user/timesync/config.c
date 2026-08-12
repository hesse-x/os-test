/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#include "config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int bad(char *error, size_t size, const char *message) {
  if (size)
    snprintf(error, size, "%s", message);
  errno = EINVAL;
  return -1;
}

static int parse_u32(const char *text, uint32_t min, uint32_t max,
                     uint32_t *out) {
  char *end;
  errno = 0;
  unsigned long value = strtoul(text, &end, 10);
  if (errno || !*text || *end || value < min || value > max)
    return -1;
  *out = (uint32_t)value;
  return 0;
}

int timesync_config_load(const char *path, struct timesync_config *config,
                         char *error, size_t error_size) {
  if (!path || !config)
    return bad(error, error_size, "invalid arguments");
  FILE *file = fopen(path, "rb");
  if (!file)
    return -1;
  struct timesync_config out = {
      .request_timeout_ms = 1500,
      .total_budget_ms = 6000,
      .sample_count = 3,
      .policy = {.min_unix_sec = 1735689600LL,
                 .max_unix_sec = 4107542399LL,
                 .max_network_delay_ns = 1000000000LL,
                 .max_server_processing_ns = 1000000000LL,
                 .max_root_delay_ns = 2000000000LL,
                 .max_root_dispersion_ns = 2000000000LL,
                 .max_offset_spread_ns = 250000000LL,
                 .negative_delay_tolerance_ns = 1000000LL,
                 .min_valid_samples = 2}};
  char line[160];
  unsigned seen = 0, lines = 0;
  while (fgets(line, sizeof(line), file)) {
    if (++lines > 32) {
      fclose(file);
      return bad(error, error_size, "too many lines");
    }
    size_t length = strlen(line);
    if (!length || (line[length - 1] != '\n' && !feof(file))) {
      fclose(file);
      return bad(error, error_size, "line too long");
    }
    while (length && (line[length - 1] == '\n' || line[length - 1] == '\r'))
      line[--length] = '\0';
    if (!length || line[0] == '#')
      continue;
    char *equal = strchr(line, '=');
    if (!equal || equal == line || !equal[1] || strchr(equal + 1, '=')) {
      fclose(file);
      return bad(error, error_size, "invalid assignment");
    }
    *equal++ = '\0';
    uint32_t value, bit = 0;
    if (!strcmp(line, "server")) {
      bit = 1u;
      struct in_addr address;
      if (inet_pton(AF_INET, equal, &address) != 1) {
        fclose(file);
        return bad(error, error_size, "server must be IPv4");
      }
      uint32_t host = ntohl(address.s_addr);
      unsigned first = host >> 24;
      if (!host || host == UINT32_MAX || first == 0 || first == 127 ||
          first >= 224) {
        fclose(file);
        return bad(error, error_size, "server is not unicast");
      }
      out.server_addr_be = address.s_addr;
      snprintf(out.server, sizeof(out.server), "%s", equal);
    } else if (!strcmp(line, "request_timeout_ms")) {
      bit = 2u;
      if (parse_u32(equal, 100, 30000, &out.request_timeout_ms))
        goto invalid_number;
    } else if (!strcmp(line, "total_budget_ms")) {
      bit = 4u;
      if (parse_u32(equal, 500, 60000, &out.total_budget_ms))
        goto invalid_number;
    } else if (!strcmp(line, "sample_count")) {
      bit = 8u;
      if (parse_u32(equal, 1, TIMESYNC_MAX_SAMPLES, &value))
        goto invalid_number;
      out.sample_count = value;
    } else if (!strcmp(line, "min_valid_samples")) {
      bit = 16u;
      if (parse_u32(equal, 1, TIMESYNC_MAX_SAMPLES, &value))
        goto invalid_number;
      out.policy.min_valid_samples = value;
    } else if (!strcmp(line, "max_network_delay_ms")) {
      bit = 32u;
      if (parse_u32(equal, 1, 30000, &value))
        goto invalid_number;
      out.policy.max_network_delay_ns = (int64_t)value * 1000000;
    } else if (!strcmp(line, "max_offset_spread_ms")) {
      bit = 64u;
      if (parse_u32(equal, 1, 10000, &value))
        goto invalid_number;
      out.policy.max_offset_spread_ns = (int64_t)value * 1000000;
    } else {
      fclose(file);
      return bad(error, error_size, "unknown key");
    }
    if (seen & bit) {
      fclose(file);
      return bad(error, error_size, "duplicate key");
    }
    seen |= bit;
    continue;
  invalid_number:
    fclose(file);
    return bad(error, error_size, "invalid numeric value");
  }
  if (ferror(file)) {
    fclose(file);
    return -1;
  }
  fclose(file);
  if (!(seen & 1u))
    return bad(error, error_size, "missing server");
  if (out.policy.min_valid_samples > out.sample_count)
    return bad(error, error_size, "sample quorum exceeds count");
  if (out.total_budget_ms < out.request_timeout_ms)
    return bad(error, error_size, "budget shorter than request timeout");
  *config = out;
  return 0;
}

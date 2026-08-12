/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef XOS_TIMESYNC_SNTP_H
#define XOS_TIMESYNC_SNTP_H

#include <stddef.h>
#include <stdint.h>

#define SNTP_PACKET_MIN 48u
#define SNTP_PACKET_MAX 512u

enum sntp_reject_reason {
  SNTP_ACCEPT = 0,
  SNTP_REJECT_LENGTH,
  SNTP_REJECT_HEADER,
  SNTP_REJECT_UNSYNCED,
  SNTP_REJECT_STRATUM,
  SNTP_REJECT_KOD,
  SNTP_REJECT_COOKIE,
  SNTP_REJECT_TIMESTAMP,
  SNTP_REJECT_RANGE,
  SNTP_REJECT_QUALITY,
  SNTP_REJECT_OVERFLOW,
  SNTP_REJECT_SPREAD,
  SNTP_REJECT_QUORUM
};

struct sntp_policy {
  int64_t min_unix_sec;
  int64_t max_unix_sec;
  int64_t max_network_delay_ns;
  int64_t max_server_processing_ns;
  int64_t max_root_delay_ns;
  int64_t max_root_dispersion_ns;
  int64_t max_offset_spread_ns;
  int64_t negative_delay_tolerance_ns;
  unsigned min_valid_samples;
};

struct sntp_expect {
  uint64_t cookie;
  int64_t t1_unix_ns;
  int64_t t4_unix_ns;
  const struct sntp_policy *policy;
};

struct sntp_sample {
  int64_t offset_ns;
  int64_t delay_ns;
  int64_t server_processing_ns;
  int64_t server_tx_unix_ns;
  uint8_t leap;
  uint8_t version;
  uint8_t stratum;
  char kod[5];
};

struct sntp_result {
  struct sntp_sample sample;
  size_t selected_index;
  size_t valid_count;
};

int sntp_build_request(uint64_t cookie, uint8_t out[SNTP_PACKET_MIN]);
int sntp_parse_response(const uint8_t *buf, size_t len,
                        const struct sntp_expect *expect,
                        struct sntp_sample *sample,
                        enum sntp_reject_reason *reason);
int sntp_select_sample(const struct sntp_sample *samples, size_t count,
                       const struct sntp_policy *policy,
                       struct sntp_result *result,
                       enum sntp_reject_reason *reason);
const char *sntp_reject_name(enum sntp_reject_reason reason);

#endif

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#include "sntp.h"

#include <limits.h>
#include <string.h>

#define NTP_UNIX_DELTA 2208988800ULL
#define NS_PER_SEC 1000000000LL

static uint32_t load_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t load_be64(const uint8_t *p) {
  return ((uint64_t)load_be32(p) << 32) | load_be32(p + 4);
}

static void store_be64(uint8_t *p, uint64_t value) {
  for (unsigned i = 0; i < 8; ++i)
    p[i] = (uint8_t)(value >> (56u - i * 8u));
}

static int fail(enum sntp_reject_reason value,
                enum sntp_reject_reason *reason) {
  if (reason)
    *reason = value;
  return -1;
}

static int add64(int64_t a, int64_t b, int64_t *out) {
  return __builtin_add_overflow(a, b, out) ? -1 : 0;
}

static int sub64(int64_t a, int64_t b, int64_t *out) {
  return __builtin_sub_overflow(a, b, out) ? -1 : 0;
}

static int ntp_to_unix_ns(uint64_t stamp, const struct sntp_policy *policy,
                          int64_t *out) {
  uint64_t seconds32 = stamp >> 32;
  uint64_t fraction = (uint32_t)stamp;
  int matches = 0;
  int64_t selected = 0;
  for (uint64_t era = 0; era <= 1; ++era) {
    uint64_t ntp_seconds = seconds32 + (era << 32);
    if (ntp_seconds < NTP_UNIX_DELTA)
      continue;
    uint64_t unix_seconds = ntp_seconds - NTP_UNIX_DELTA;
    if (unix_seconds > (uint64_t)INT64_MAX / NS_PER_SEC)
      continue;
    int64_t ns = (int64_t)unix_seconds * NS_PER_SEC;
    ns += (int64_t)(((__uint128_t)fraction * NS_PER_SEC) >> 32);
    int64_t sec = ns / NS_PER_SEC;
    if (sec >= policy->min_unix_sec && sec <= policy->max_unix_sec) {
      selected = ns;
      ++matches;
    }
  }
  if (matches != 1)
    return -1;
  *out = selected;
  return 0;
}

int sntp_build_request(uint64_t cookie, uint8_t out[SNTP_PACKET_MIN]) {
  if (!cookie || !out)
    return -1;
  memset(out, 0, SNTP_PACKET_MIN);
  out[0] = (4u << 3) | 3u;
  store_be64(out + 40, cookie);
  return 0;
}

int sntp_parse_response(const uint8_t *buf, size_t len,
                        const struct sntp_expect *expect,
                        struct sntp_sample *sample,
                        enum sntp_reject_reason *reason) {
  if (reason)
    *reason = SNTP_ACCEPT;
  if (!buf || !expect || !sample || !expect->policy || len < SNTP_PACKET_MIN ||
      len > SNTP_PACKET_MAX)
    return fail(SNTP_REJECT_LENGTH, reason);
  uint8_t leap = buf[0] >> 6;
  uint8_t version = (buf[0] >> 3) & 7u;
  uint8_t mode = buf[0] & 7u;
  uint8_t stratum = buf[1];
  if ((version != 3 && version != 4) || mode != 4)
    return fail(SNTP_REJECT_HEADER, reason);
  if (leap == 3)
    return fail(SNTP_REJECT_UNSYNCED, reason);
  if (stratum == 0) {
    memset(sample, 0, sizeof(*sample));
    memcpy(sample->kod, buf + 12, 4);
    sample->kod[4] = '\0';
    return fail(SNTP_REJECT_KOD, reason);
  }
  if (stratum > 15)
    return fail(SNTP_REJECT_STRATUM, reason);
  if (load_be64(buf + 24) != expect->cookie)
    return fail(SNTP_REJECT_COOKIE, reason);
  uint64_t receive = load_be64(buf + 32);
  uint64_t transmit = load_be64(buf + 40);
  if (!receive || !transmit)
    return fail(SNTP_REJECT_TIMESTAMP, reason);

  const struct sntp_policy *p = expect->policy;
  int64_t t2, t3;
  if (ntp_to_unix_ns(receive, p, &t2) || ntp_to_unix_ns(transmit, p, &t3))
    return fail(SNTP_REJECT_RANGE, reason);
  int64_t processing;
  if (sub64(t3, t2, &processing) || processing < 0)
    return fail(SNTP_REJECT_TIMESTAMP, reason);
  if (processing > p->max_server_processing_ns)
    return fail(SNTP_REJECT_QUALITY, reason);

  int64_t local_elapsed, delay;
  if (sub64(expect->t4_unix_ns, expect->t1_unix_ns, &local_elapsed) ||
      sub64(local_elapsed, processing, &delay))
    return fail(SNTP_REJECT_OVERFLOW, reason);
  if (delay < -p->negative_delay_tolerance_ns ||
      delay > p->max_network_delay_ns)
    return fail(SNTP_REJECT_QUALITY, reason);
  if (delay < 0)
    delay = 0;

  int64_t a, b, sum;
  if (sub64(t2, expect->t1_unix_ns, &a) || sub64(t3, expect->t4_unix_ns, &b) ||
      add64(a, b, &sum))
    return fail(SNTP_REJECT_OVERFLOW, reason);

  int64_t root_delay = (int64_t)(int32_t)load_be32(buf + 4);
  int64_t root_dispersion = load_be32(buf + 8);
  root_delay = (int64_t)(((__int128)root_delay * NS_PER_SEC) / 65536);
  root_dispersion = (int64_t)(((__int128)root_dispersion * NS_PER_SEC) / 65536);
  if (root_delay < 0 || root_delay > p->max_root_delay_ns ||
      root_dispersion > p->max_root_dispersion_ns)
    return fail(SNTP_REJECT_QUALITY, reason);

  memset(sample, 0, sizeof(*sample));
  sample->offset_ns = sum / 2;
  sample->delay_ns = delay;
  sample->server_processing_ns = processing;
  sample->server_tx_unix_ns = t3;
  sample->leap = leap;
  sample->version = version;
  sample->stratum = stratum;
  return 0;
}

int sntp_select_sample(const struct sntp_sample *samples, size_t count,
                       const struct sntp_policy *policy,
                       struct sntp_result *result,
                       enum sntp_reject_reason *reason) {
  if (!samples || !policy || !result || count < policy->min_valid_samples)
    return fail(SNTP_REJECT_QUORUM, reason);
  int64_t minimum = samples[0].offset_ns, maximum = minimum;
  size_t best = 0;
  for (size_t i = 1; i < count; ++i) {
    if (samples[i].offset_ns < minimum)
      minimum = samples[i].offset_ns;
    if (samples[i].offset_ns > maximum)
      maximum = samples[i].offset_ns;
    if (samples[i].delay_ns < samples[best].delay_ns)
      best = i;
  }
  int64_t spread;
  if (sub64(maximum, minimum, &spread) || spread > policy->max_offset_spread_ns)
    return fail(SNTP_REJECT_SPREAD, reason);
  result->sample = samples[best];
  result->selected_index = best;
  result->valid_count = count;
  if (reason)
    *reason = SNTP_ACCEPT;
  return 0;
}

const char *sntp_reject_name(enum sntp_reject_reason reason) {
  static const char *const names[] = {
      "accepted",      "length", "header",    "unsynchronized", "stratum",
      "kiss-of-death", "cookie", "timestamp", "range",          "quality",
      "overflow",      "spread", "quorum"};
  return (unsigned)reason < sizeof(names) / sizeof(names[0]) ? names[reason]
                                                             : "unknown";
}

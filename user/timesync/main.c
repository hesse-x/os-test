/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#include "../netd/control_protocol.h"
#include "config.h"
#include "sntp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define NETD_CONTROL_PATH "/run/netd/control.sock"
#define STATUS_PATH "/run/timesync/status"
#define READY_PATH "/run/timesync/ready"
#define NS_PER_SEC 1000000000LL

static int64_t timespec_ns(const struct timespec *value) {
  return (int64_t)value->tv_sec * NS_PER_SEC + value->tv_nsec;
}

static int read_exact(int fd, void *buffer, size_t length) {
  uint8_t *p = buffer;
  while (length) {
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
    ssize_t n = write(fd, p, length);
    if (n <= 0)
      return -1;
    p += n;
    length -= (size_t)n;
  }
  return 0;
}

static int random_cookie(uint64_t *cookie) {
  do {
    size_t done = 0;
    while (done < sizeof(*cookie)) {
      ssize_t n =
          getrandom((uint8_t *)cookie + done, sizeof(*cookie) - done, 0);
      if (n <= 0)
        return -1;
      done += (size_t)n;
    }
  } while (!*cookie);
  return 0;
}

static int exchange(const struct timesync_config *config, uint32_t request_id,
                    uint32_t epoch, uint64_t cookie,
                    struct netd_sntp4_result *result, uint32_t *new_epoch) {
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;
  struct sockaddr_un address = {.sun_family = AF_UNIX};
  strncpy(address.sun_path, NETD_CONTROL_PATH, sizeof(address.sun_path) - 1);
  if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
    close(fd);
    return -1;
  }
  struct netd_sntp4_request request = {.server_addr_be = config->server_addr_be,
                                       .timeout_ms = config->request_timeout_ms,
                                       .request_len = NETD_SNTP_REQUEST_LEN};
  if (sntp_build_request(cookie, request.request)) {
    close(fd);
    errno = EINVAL;
    return -1;
  }
  struct netd_ctl_header header = {.magic = NETD_CTL_MAGIC,
                                   .version = NETD_CTL_VERSION,
                                   .op = NETD_CTL_SNTP4_EXCHANGE,
                                   .request_id = request_id,
                                   .payload_len = sizeof(request),
                                   .net_epoch = epoch};
  struct netd_ctl_response response;
  if (write_exact(fd, &header, sizeof(header)) ||
      write_exact(fd, &request, sizeof(request)) ||
      read_exact(fd, &response, sizeof(response))) {
    close(fd);
    return -1;
  }
  if (response.header.magic != NETD_CTL_MAGIC ||
      response.header.version != NETD_CTL_VERSION ||
      response.header.op != NETD_CTL_SNTP4_EXCHANGE ||
      response.header.request_id != request_id || response.header.flags) {
    close(fd);
    errno = EPROTO;
    return -1;
  }
  *new_epoch = response.header.net_epoch;
  if (response.status != NETD_CTL_OK) {
    close(fd);
    errno = response.status == NETD_CTL_TIMEOUT ? ETIMEDOUT
            : response.status == NETD_CTL_STALE ? ESTALE
                                                : EHOSTUNREACH;
    return -1;
  }
  if (response.header.payload_len <
          offsetof(struct netd_sntp4_result, response) ||
      response.header.payload_len > sizeof(*result)) {
    close(fd);
    errno = EPROTO;
    return -1;
  }
  memset(result, 0, sizeof(*result));
  if (read_exact(fd, result, response.header.payload_len)) {
    close(fd);
    return -1;
  }
  close(fd);
  size_t expected =
      offsetof(struct netd_sntp4_result, response) + result->response_len;
  if (expected != response.header.payload_len ||
      result->response_len > NETD_SNTP_RESPONSE_MAX ||
      result->server_addr_be != config->server_addr_be ||
      result->server_port_be != htons(123) ||
      result->recv_mono_ns < result->send_mono_ns) {
    errno = EPROTO;
    return -1;
  }
  return 0;
}

static int publish(const char *body, bool ready) {
  mkdir("/run/timesync", 0755);
  unlink(READY_PATH);
  char temporary[64];
  snprintf(temporary, sizeof(temporary), "/run/timesync/status.%d", getpid());
  int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (fd < 0)
    return -1;
  size_t length = strlen(body);
  int rc = write_exact(fd, body, length) || fsync(fd) || close(fd) ||
           rename(temporary, STATUS_PATH);
  if (rc) {
    close(fd);
    unlink(temporary);
    return -1;
  }
  if (!ready)
    return 0;
  fd = open(READY_PATH, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (fd < 0)
    return -1;
  return close(fd);
}

static int show_status(void) {
  int fd = open(STATUS_PATH, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return 1;
  char buffer[1024];
  ssize_t n = read(fd, buffer, sizeof(buffer));
  close(fd);
  if (n <= 0 || write_exact(STDOUT_FILENO, buffer, (size_t)n))
    return 1;
  return 0;
}

int main(int argc, char **argv) {
  bool dry_run = false;
  if (argc == 2 && !strcmp(argv[1], "--status"))
    return show_status();
  if (argc == 2 && !strcmp(argv[1], "--dry-run"))
    dry_run = true;
  else if (argc != 1) {
    fprintf(stderr, "usage: timesync [--status|--dry-run]\n");
    return 3;
  }
  struct timesync_config config;
  char error[128];
  if (timesync_config_load(TIMESYNC_CONFIG_PATH, &config, error,
                           sizeof(error))) {
    fprintf(stderr, "timesync: config: %s\n", errno ? strerror(errno) : error);
    return 3;
  }
  struct timespec start_mono, start_real;
  if (clock_gettime(CLOCK_MONOTONIC, &start_mono) ||
      clock_gettime(CLOCK_REALTIME, &start_real))
    return 3;
  int64_t anchor = timespec_ns(&start_real) - timespec_ns(&start_mono);
  uint64_t total_deadline = (uint64_t)timespec_ns(&start_mono) +
                            (uint64_t)config.total_budget_ms * 1000000ULL;
  struct sntp_sample samples[TIMESYNC_MAX_SAMPLES];
  unsigned valid = 0;
  uint32_t epoch = 0;
  enum sntp_reject_reason last_reason = SNTP_REJECT_QUORUM;
  for (unsigned i = 0; i < config.sample_count; ++i) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if ((uint64_t)timespec_ns(&now) >= total_deadline)
      break;
    uint64_t cookie;
    if (random_cookie(&cookie))
      return 3;
    struct netd_sntp4_result wire;
    uint32_t response_epoch;
    if (exchange(&config, i + 1, epoch, cookie, &wire, &response_epoch)) {
      fprintf(stderr, "timesync: sample %u transport: %s\n", i,
              strerror(errno));
      if (errno == ESTALE)
        return 2;
      continue;
    }
    if (!epoch)
      epoch = response_epoch;
    if (epoch != response_epoch)
      return 2;
    struct sntp_expect expect = {
        .cookie = cookie,
        .t1_unix_ns = anchor + (int64_t)wire.send_mono_ns,
        .t4_unix_ns = anchor + (int64_t)wire.recv_mono_ns,
        .policy = &config.policy};
    if (sntp_parse_response(wire.response, wire.response_len, &expect,
                            &samples[valid], &last_reason)) {
      fprintf(stderr, "timesync: sample %u rejected: %s\n", i,
              sntp_reject_name(last_reason));
      if (last_reason == SNTP_REJECT_KOD)
        return 4;
      continue;
    }
    ++valid;
  }
  struct sntp_result selected;
  if (sntp_select_sample(samples, valid, &config.policy, &selected,
                         &last_reason)) {
    char body[256];
    snprintf(body, sizeof(body),
             "state=FAILED\nserver=%s\nreason=%s\nvalid_samples=%u\n",
             config.server, sntp_reject_name(last_reason), valid);
    (void)publish(body, false);
    return 4;
  }
  struct timespec decision_mono, decision_real;
  clock_gettime(CLOCK_MONOTONIC, &decision_mono);
  clock_gettime(CLOCK_REALTIME, &decision_real);
  int64_t decision_anchor =
      timespec_ns(&decision_real) - timespec_ns(&decision_mono);
  if (llabs(decision_anchor - anchor) > 50000000LL) {
    fprintf(stderr, "timesync: realtime changed during sampling\n");
    return 2;
  }
  int64_t target_ns = timespec_ns(&decision_real) + selected.sample.offset_ns;
  if (target_ns < 0)
    return 4;
  struct timespec target = {.tv_sec = target_ns / NS_PER_SEC,
                            .tv_nsec = target_ns % NS_PER_SEC};
  if (!dry_run && clock_settime(CLOCK_REALTIME, &target)) {
    fprintf(stderr, "timesync: clock_settime: %s\n", strerror(errno));
    return errno == EPERM ? 3 : 4;
  }
  struct timespec readback;
  clock_gettime(CLOCK_REALTIME, &readback);
  if (!dry_run && llabs(timespec_ns(&readback) - target_ns) > 100000000LL)
    return 4;
  char body[512];
  snprintf(body, sizeof(body),
           "state=%s\nserver=%s\nnet_epoch=%u\nsync_mono_ns=%lld\n"
           "target_unix_sec=%lld\noffset_ns=%lld\ndelay_ns=%lld\n"
           "valid_samples=%u\n",
           dry_run ? "DRY_RUN" : "SYNCED", config.server, epoch,
           (long long)timespec_ns(&decision_mono), (long long)target.tv_sec,
           (long long)selected.sample.offset_ns,
           (long long)selected.sample.delay_ns, valid);
  if (publish(body, !dry_run))
    return 4;
  return 0;
}

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <xos/perf.h>
#include <xos/syscall_nums.h>

#include "perf_config.h"

#define EXPORT_CHUNK (XOS_PERF_MAX_READ)
#define CHECKPOINT_INTERVAL_SECONDS 60U
#define PERF_RAW_FINAL "/var/perf/PERF.RAW"
#define PERF_RAW_TEMP "/var/perf/PERF.TMP"
#define PERF_METADATA_FINAL "/var/perf/METADATA.JSO"
#define PERF_METADATA_TEMP "/var/perf/META.TMP"

static uint8_t export_buffer[EXPORT_CHUNK];

static long perf_call(long cmd, long arg1, long arg2, long arg3) {
  register long r10 __asm__("r10") = arg3;
  register long r8 __asm__("r8") = 0;
  register long r9 __asm__("r9") = 0;
  long result;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"((long)SYS_PERF), "D"(cmd), "S"(arg1), "d"(arg2),
                     "r"(r10), "r"(r8), "r"(r9)
                   : "rcx", "r11", "memory");
  if ((unsigned long)result > (unsigned long)-4096) {
    errno = (int)-result;
    return -1;
  }
  return result;
}

static int write_all(int fd, const void *buffer, size_t length) {
  const uint8_t *p = buffer;
  while (length != 0) {
    ssize_t written = write(fd, p, length);
    if (written < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (written == 0) {
      errno = EIO;
      return -1;
    }
    p += written;
    length -= (size_t)written;
  }
  return 0;
}

static int export_raw(const struct xos_perf_info *info, int final) {
  const char *path = final ? PERF_RAW_FINAL : PERF_RAW_TEMP;
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    perror("perf: open raw snapshot");
    return -1;
  }

  uint64_t offset = 0;
  while (offset < info->raw_size) {
    size_t request = sizeof(export_buffer);
    if (request > info->raw_size - offset)
      request = (size_t)(info->raw_size - offset);
    long got = perf_call(XOS_PERF_READ, (long)export_buffer, (long)request,
                         (long)offset);
    if (got <= 0 || (size_t)got > request) {
      if (got > 0)
        errno = EOVERFLOW;
      perror("perf: read raw snapshot");
      close(fd);
      return -1;
    }
    if (write_all(fd, export_buffer, (size_t)got) < 0) {
      perror("perf: write raw snapshot");
      close(fd);
      return -1;
    }
    offset += (uint64_t)got;
  }
  if (fsync(fd) < 0) {
    perror("perf: fsync raw snapshot");
    close(fd);
    return -1;
  }
  if (close(fd) < 0) {
    perror("perf: close raw snapshot");
    return -1;
  }
  return 0;
}

static int export_metadata(const struct xos_perf_info *info,
                           const struct xos_perf_metadata *metadata,
                           int runner_status, int final) {
  char json[768];
  int length = snprintf(
      json, sizeof(json),
      "{\n  \"abi_version\": %u,\n  \"complete\": %s,\n"
      "  \"end_reason\": %llu,\n  \"runner_status\": %d,\n"
      "  \"boot_tsc\": %llu,\n  \"tsc_freq\": %llu,\n"
      "  \"record_count\": %llu,\n  \"committed_bytes\": %llu,\n"
      "  \"sampling_source\": %u,\n  \"pmu_active_mask\": %u,\n"
      "  \"nmi_count\": %llu,\n  \"handler_cycles\": %llu,\n"
      "  \"truncated_callchains\": %llu,\n  \"trace_lost\": %llu\n}\n",
      metadata->abi_version, (info->flags & 1U) ? "true" : "false",
      (unsigned long long)info->end_reason, runner_status,
      (unsigned long long)metadata->boot_tsc,
      (unsigned long long)metadata->tsc_freq,
      (unsigned long long)metadata->record_count,
      (unsigned long long)metadata->committed_bytes, metadata->sampling_source,
      metadata->pmu_active_mask, (unsigned long long)metadata->nmi_count,
      (unsigned long long)metadata->handler_cycles,
      (unsigned long long)metadata->truncated_callchains,
      (unsigned long long)metadata->trace_lost);
  if (length < 0 || (size_t)length >= sizeof(json)) {
    errno = EOVERFLOW;
    return -1;
  }

  const char *path = final ? PERF_METADATA_FINAL : PERF_METADATA_TEMP;
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    perror("perf: open metadata snapshot");
    return -1;
  }
  if (write_all(fd, json, (size_t)length) < 0) {
    perror("perf: write metadata snapshot");
    close(fd);
    return -1;
  }
  if (fsync(fd) < 0) {
    perror("perf: fsync metadata snapshot");
    close(fd);
    return -1;
  }
  if (close(fd) < 0) {
    perror("perf: close metadata snapshot");
    return -1;
  }
  return 0;
}

static int persist_snapshot(int runner_status, int final) {
  struct xos_perf_info info;
  struct xos_perf_metadata metadata;
  if (perf_call(XOS_PERF_GET_INFO, (long)&info, sizeof(info), 0) < 0) {
    perror("perf: snapshot GET_INFO");
    return -1;
  }
  if (perf_call(XOS_PERF_GET_METADATA, (long)&metadata, sizeof(metadata), 0) <
      0) {
    perror("perf: snapshot GET_METADATA");
    return -1;
  }
  if (info.raw_size == 0) {
    errno = ENODATA;
    perror("perf: empty snapshot");
    return -1;
  }
  if (export_raw(&info, final) < 0 ||
      export_metadata(&info, &metadata, runner_status, final) < 0)
    return -1;
  sync();
  printf("perf: %s %llu bytes, %llu records, complete=%u status=%d\n",
         final ? "exported" : "checkpointed", (unsigned long long)info.raw_size,
         (unsigned long long)metadata.record_count, info.flags & 1U,
         runner_status);
  return 0;
}

int main(int argc, char **argv) {
  const char *target = argc > 1 ? argv[1] : "/test/test_runner.elf";
  const char *test_name = PERF_TEST_NAME;
  struct xos_perf_info info;
  if (perf_call(XOS_PERF_GET_INFO, (long)&info, sizeof(info), 0) < 0) {
    perror("perf: GET_INFO");
    return 1;
  }
  if ((mkdir("/var", 0755) < 0 && errno != EEXIST) ||
      (mkdir("/var/perf", 0755) < 0 && errno != EEXIST)) {
    perror("perf: mkdir");
    return 1;
  }

  // Export the boot snapshot before starting the target so profiling I/O does
  // not compete with the workload for the same FAT-backed block device.
  if (perf_call(XOS_PERF_CHECKPOINT, 0, 0, 0) < 0) {
    perror("perf: initial checkpoint");
    return 1;
  }
  if (persist_snapshot(-1, 0) < 0) {
    perror("perf: initial snapshot");
    return 1;
  }

  int start_pipe[2];
  if (pipe(start_pipe) < 0) {
    perror("perf: start pipe");
    return 1;
  }

  pid_t runner = fork();
  if (runner == 0) {
    close(start_pipe[1]);
    uint8_t start_token;
    ssize_t got;
    do {
      got = read(start_pipe[0], &start_token, sizeof(start_token));
    } while (got < 0 && errno == EINTR);
    close(start_pipe[0]);
    if (got != (ssize_t)sizeof(start_token))
      _exit(126);

    char *env[] = {"XOS_SKIP_AUTOTEST=1", NULL};
    char *target_argv[] = {(char *)target,
                           test_name[0] ? (char *)test_name : NULL, NULL};
    execve(target, target_argv, env);
    _exit(127);
  }
  if (runner < 0) {
    close(start_pipe[0]);
    close(start_pipe[1]);
    perror("perf: fork runner");
    return 1;
  }
  close(start_pipe[0]);
  if (perf_call(XOS_PERF_REGISTER_TARGET, runner, 0, 0) < 0) {
    close(start_pipe[1]);
    kill(runner, SIGKILL);
    waitpid(runner, NULL, 0);
    perror("perf: REGISTER_TARGET");
    return 1;
  }
  uint8_t start_token = 1;
  if (write_all(start_pipe[1], &start_token, sizeof(start_token)) < 0 ||
      close(start_pipe[1]) < 0) {
    kill(runner, SIGKILL);
    waitpid(runner, NULL, 0);
    perror("perf: start runner");
    return 1;
  }

  int runner_status = 0;
  unsigned checkpoint_seconds = 0;
  for (;;) {
    pid_t waited = waitpid(runner, &runner_status, WNOHANG);
    if (waited == runner)
      break;
    if (waited < 0) {
      if (errno == EINTR)
        continue;
      perror("perf: waitpid");
      return 1;
    }
    sleep(1);
    if (perf_call(XOS_PERF_GET_INFO, (long)&info, sizeof(info), 0) < 0) {
      perror("perf: watchdog poll");
      return 1;
    }
    if (info.state == XOS_PERF_FROZEN) {
      kill(runner, SIGKILL);
      waitpid(runner, &runner_status, 0);
      break;
    }
    checkpoint_seconds++;
    if (checkpoint_seconds < CHECKPOINT_INTERVAL_SECONDS)
      continue;
    checkpoint_seconds = 0;
    if (perf_call(XOS_PERF_CHECKPOINT, 0, 0, 0) == 0 &&
        persist_snapshot(-1, 0) < 0) {
      perror("perf: periodic checkpoint");
      return 1;
    }
  }

  if (perf_call(XOS_PERF_GET_INFO, (long)&info, sizeof(info), 0) < 0) {
    perror("perf: GET_INFO frozen");
    return 1;
  }
  if (info.state != XOS_PERF_FROZEN) {
    long complete = runner_status == 0 ? 1 : 0;
    if (perf_call(XOS_PERF_FREEZE, XOS_PERF_END_MANUAL, complete, 0) < 0 ||
        perf_call(XOS_PERF_GET_INFO, (long)&info, sizeof(info), 0) < 0) {
      perror("perf: FREEZE");
      return 1;
    }
  }

  if (persist_snapshot(runner_status, 1) < 0) {
    perror("perf: export");
    return 1;
  }
  perf_call(XOS_PERF_REQUEST_EXIT, 0, 0, 0);
  return 0;
}

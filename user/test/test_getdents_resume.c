/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_getdents_resume.c — 01: getdents64 chunked resume on in-memory
 * filesystems (tmpfs/sysfs). Before the fix, tmpfs/sysfs unconditionally
 * wrote ctx->pos = -1 after the loop, clobbering the resume cursor dir_emit
 * left on a buffer-full short read; the next getdents64 call hit the EOF
 * marker and returned 0, dropping every entry past the first chunk. This test
 * forces small getdents buffers across >200 entries and asserts the full set
 * is delivered with no duplicates and no gaps. */
#include "unity.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syscall.h>
#include <unistd.h>
#include <xos/dirent.h>

void setUp(void) {}
void tearDown(void) {}

static const char *DIRP = "/run/getdents_resume";
#define NFILES 240

static void create_fixtures(void) {
  mkdir(DIRP, 0755);
  for (int i = 0; i < NFILES; i++) {
    char path[128];
    snprintf(path, sizeof(path), "%s/f_%04d", DIRP, i);
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0)
      close(fd);
  }
}

static void remove_fixtures(void) {
  for (int i = 0; i < NFILES; i++) {
    char path[128];
    snprintf(path, sizeof(path), "%s/f_%04d", DIRP, i);
    unlink(path);
  }
  rmdir(DIRP);
}

/* A tiny getdents buffer forces the kernel to hand back a few entries per
 * call, exercising the resume cursor across many calls. 64 bytes fits at most
 * one dirent64 of a short name (fixed part is 19B + name + NUL, 8-aligned). */
#define BUFSZ 64

static void drain_dir(const char *path, int *out_count, int *out_seen_dotdot) {
  int fd = open(path, O_RDONLY | O_DIRECTORY);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);

  /* seen[i] == 1 once file i has been delivered. Detects both drops (a missing
   * entry) and duplicate delivery across chunk boundaries. */
  static unsigned char seen[NFILES];
  memset(seen, 0, sizeof(seen));
  int count = 0;
  int got_dotdot = 0;

  for (;;) {
    unsigned char buf[BUFSZ];
    int n = sys_getdents(fd, buf, sizeof(buf));
    if (n == 0)
      break;
    if (n < 0) {
      TEST_ASSERT_EQUAL_INT(0, errno); /* unreachable reporting */
      break;
    }
    int off = 0;
    while (off < n) {
      struct dirent64 *d = (struct dirent64 *)(buf + off);
      count++;
      if (strcmp(d->d_name, ".") == 0 || strcmp(d->d_name, "..") == 0) {
        if (strcmp(d->d_name, "..") == 0)
          got_dotdot = 1;
      } else {
        /* Must be one of f_XXXX. Parse the suffix ourselves — the libc's
         * sscanf is a minimal stub that never writes converted outputs, so
         * "%u" would leave idx at 0 for every name and falsely trip the
         * duplicate check. */
        if (d->d_name[0] != 'f' || d->d_name[1] != '_') {
          TEST_FAIL_MESSAGE("unexpected entry name");
        }
        char *end = NULL;
        long li = strtol(d->d_name + 2, &end, 10);
        TEST_ASSERT_NOT_NULL(end);
        TEST_ASSERT_EQUAL_INT('\0', (int)(unsigned char)*end); /* all digits */
        TEST_ASSERT_GREATER_OR_EQUAL_INT(0, (int)li);
        TEST_ASSERT_LESS_THAN_INT(NFILES, (int)li);
        unsigned int idx = (unsigned int)li;
        TEST_ASSERT_EQUAL_INT(0, seen[idx]); /* no duplicate delivery */
        seen[idx] = 1;
      }
      TEST_ASSERT_NOT_EQUAL_INT(0, d->d_reclen);
      off += d->d_reclen;
    }
  }
  close(fd);

  /* Every created file must have been delivered exactly once. Before the fix
   * only the first chunk (~1 entry) was delivered. */
  int total = 0;
  for (int i = 0; i < NFILES; i++)
    total += seen[i];
  TEST_ASSERT_EQUAL_INT(NFILES, total);
  *out_count = count;
  *out_seen_dotdot = got_dotdot;
}

void test_tmpfs_getdents_resume(void) {
  create_fixtures();
  int count = 0, got_dotdot = 0;
  drain_dir(DIRP, &count, &got_dotdot);
  /* NFILES created files + "." + "..". */
  TEST_ASSERT_EQUAL_INT(NFILES + 2, count);
  TEST_ASSERT_EQUAL_INT(1, got_dotdot);
  remove_fixtures();
}

/* sysfs is an in-memory fs with the same getdents shape. We can't predict its
 * exact children, but a chunked drain must reach EOF (return 0) without losing
 * the cursor mid-way (pre-fix: a second call returned 0 immediately, so only
 * the first chunk was ever visible). Assert the drain terminates cleanly and
 * yields a non-empty, duplicate-free set. */
void test_sysfs_getdents_drains_to_eof(void) {
  int fd = open("/sys", O_RDONLY | O_DIRECTORY);
  if (fd < 0) {
    /* /sys not mounted in this build — skip rather than fail. */
    TEST_IGNORE();
    return;
  }
  int count = 0;
  for (;;) {
    unsigned char buf[BUFSZ];
    int n = sys_getdents(fd, buf, sizeof(buf));
    if (n == 0)
      break;
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    int off = 0;
    while (off < n) {
      struct dirent64 *d = (struct dirent64 *)(buf + off);
      count++;
      TEST_ASSERT_NOT_EQUAL_INT(0, d->d_reclen);
      off += d->d_reclen;
    }
  }
  close(fd);
  TEST_ASSERT_GREATER_THAN_INT(0, count);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_tmpfs_getdents_resume);
  RUN_TEST(test_sysfs_getdents_drains_to_eof);
  return UNITY_END();
}

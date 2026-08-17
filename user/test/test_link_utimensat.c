/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// test_link_utimensat.c — verifies §3.5 utimensat(2) (in-memory inode
// timestamps, Q5). Aligned with the test_rename.c/test_stat_real.c style:
// Unity freestanding, dual fixtures of FAT32 (/ prefix) plus tmpfs (/run
// prefix).
//
// Timestamps are in-memory: inode atime/mtime/ctime (ns); getattr reads
// st_mtim/st_atim. Not persisted to FAT32 (Q5: llvm libc utimensat test
// doesn't survive restarts). UTIME_NOW/OMIT are in uapi fcntl.h.
// link/nlink assertions (Q3) are enabled in phase 3 via #ifdef TEST_LINK;
// phase 1 only covers utimensat.
//
// FAT32 vs tmpfs difference: a fat32 inode has no strong internal fs
// reference, so after utimensat releases the inode it can be recycled; a
// later stat creates a new inode via lookup → mtime reverts to the current
// time initialized by fat32_iget (not the explicit value); tmpfs children
// hold inode references and aren't recycled → the explicit value round-trips
// exactly. Therefore the explicit-value round-trip is only asserted on tmpfs;
// fat32 asserts mtime>0 (UTIME_NOW/NULL) or accepts the recycling semantics.
#include "unity.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FAT "/lut_fat"
#define TFS "/run/lut_tfs"

void setUp(void) {
  mkdir(FAT, 0755);
  mkdir(TFS, 0755);
}
void tearDown(void) {}

static void cleanup(void) {
  unlink(FAT "/f");
  unlink(TFS "/f");
}

// tmpfs: utimensat explicit value round-trips exactly (inode not recycled,
// ns=0 decomposes into sec without loss).
void test_utimensat_explicit_tmpfs(void) {
  cleanup();
  int fd = open(TFS "/f", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);

  struct timespec ts[2] = {{1700000000, 0}, {1700000000, 0}};
  TEST_ASSERT_EQUAL_INT(0, utimensat(AT_FDCWD, TFS "/f", ts, 0));

  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(TFS "/f", &st));
  TEST_ASSERT_EQUAL_INT(1700000000, (int)st.st_mtim.tv_sec);
  TEST_ASSERT_EQUAL_INT(0, (int)st.st_mtim.tv_nsec);
}

// UTIME_NOW: tv_nsec=UTIME_NOW → set to the current time. Both fat32 and
// tmpfs should return non-zero (fat32 initializes the new inode to the
// current time even if recycled; tmpfs writes now directly).
void test_utimensat_utime_now(void) {
  cleanup();
  int fd = open(FAT "/f", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);

  struct timespec ts[2] = {{0, UTIME_NOW}, {0, UTIME_NOW}};
  TEST_ASSERT_EQUAL_INT(0, utimensat(AT_FDCWD, FAT "/f", ts, 0));

  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(FAT "/f", &st));
  TEST_ASSERT_GREATER_THAN_INT(1700000000, (int)st.st_mtim.tv_sec);
}

// UTIME_OMIT (tmpfs): first set explicit 1700000000, then call with
// UTIME_OMIT, verifying mtime stays 1700000000 (UTIME_OMIT doesn't overwrite
// that field).
void test_utimensat_utime_omit(void) {
  cleanup();
  int fd = open(TFS "/f", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);

  struct timespec ts_set[2] = {{1700000000, 0}, {1700000000, 0}};
  TEST_ASSERT_EQUAL_INT(0, utimensat(AT_FDCWD, TFS "/f", ts_set, 0));

  struct timespec ts_omit[2] = {{0, UTIME_OMIT}, {0, UTIME_OMIT}};
  TEST_ASSERT_EQUAL_INT(0, utimensat(AT_FDCWD, TFS "/f", ts_omit, 0));

  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(TFS "/f", &st));
  TEST_ASSERT_EQUAL_INT(1700000000, (int)st.st_mtim.tv_sec);
}

// times=NULL → atime=mtime=now (aligned with Linux; write permission
// required). root allows W_OK. Both fat32 and tmpfs should return non-zero
// mtime.
void test_utimensat_null_times(void) {
  cleanup();
  int fd = open(FAT "/f", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);

  TEST_ASSERT_EQUAL_INT(0, utimensat(AT_FDCWD, FAT "/f", NULL, 0));

  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(FAT "/f", &st));
  TEST_ASSERT_GREATER_THAN_INT(1700000000, (int)st.st_mtim.tv_sec);
}

// atime should also be set: after UTIME_NOW sets atime, stat reads
// st_atim > 0.
void test_utimensat_atime(void) {
  cleanup();
  int fd = open(TFS "/f", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);

  struct timespec ts[2] = {{0, UTIME_NOW}, {0, UTIME_OMIT}};
  TEST_ASSERT_EQUAL_INT(0, utimensat(AT_FDCWD, TFS "/f", ts, 0));

  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(TFS "/f", &st));
  TEST_ASSERT_GREATER_THAN_INT(1700000000, (int)st.st_atim.tv_sec);
}

// Invalid flags (not AT_SYMLINK_NOFOLLOW) → EINVAL; out-of-range tv_nsec
// (>=1e9 or <0, not NOW/OMIT) → EINVAL (Q6 strict validation).
void test_utimensat_invalid_args(void) {
  cleanup();
  int fd = open(FAT "/f", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);

  TEST_ASSERT_EQUAL_INT(-1, utimensat(AT_FDCWD, FAT "/f", NULL, 0x400000));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);

  struct timespec bad[2] = {{1700000000, 2000000000}};
  TEST_ASSERT_EQUAL_INT(-1, utimensat(AT_FDCWD, FAT "/f", bad, 0));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);

  struct timespec bad2[2] = {{1700000000, -1}};
  TEST_ASSERT_EQUAL_INT(-1, utimensat(AT_FDCWD, FAT "/f", bad2, 0));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
}

// Nonexistent path → ENOENT.
void test_utimensat_noent(void) {
  cleanup();
  TEST_ASSERT_EQUAL_INT(-1, utimensat(AT_FDCWD, FAT "/nope", NULL, 0));
  TEST_ASSERT_EQUAL_INT(ENOENT, errno);
}

// ===================== link / nlink (phase 3, TEST_LINK) =====================
#ifdef TEST_LINK
// Enabled in phase 3: tmpfs hard-link nlink==2; nlink==1 after unlink;
// hard-linking a directory EPERM; cross-fs (FAT32↔tmpfs) EXDEV; FAT32 link
// EPERM. Placeholder only — filled in phase 3.
void test_link_tmpfs_nlink(void) {
  cleanup();
  TEST_IGNORE_MESSAGE("link: filled in phase 3");
}
#endif

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_utimensat_explicit_tmpfs);
  RUN_TEST(test_utimensat_utime_now);
  RUN_TEST(test_utimensat_utime_omit);
  RUN_TEST(test_utimensat_null_times);
  RUN_TEST(test_utimensat_atime);
  RUN_TEST(test_utimensat_invalid_args);
  RUN_TEST(test_utimensat_noent);
#ifdef TEST_LINK
  RUN_TEST(test_link_tmpfs_nlink);
#endif
  return UNITY_END();
}

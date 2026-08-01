/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// test_access.c — verify §3.2 access(2)/faccessat(2) (kernel inode_permission
// keyed on euid, Q4). Mirrors test_rename.c/test_stat_real.c: Unity
// freestanding, FAT32 (/ prefix) + tmpfs (/run prefix) dual fixtures.
//
// This OS defaults to euid=0=root; inode_permission lets root through
// (equivalent to CAP_DAC_OVERRIDE), so R_OK/W_OK/X_OK always return 0 for root.
// Mainly verifies existence (F_OK=0), non-existence (ENOENT), and the faccessat
// AT_FDCWD/dirfd paths. Non-root permission-bit checks live in
// test_setuid_saved.
#include "unity.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FAT "/access_fat"
#define TFS "/run/access_tfs"

void setUp(void) {
  // Fixture parent dirs (idempotent: mkdir only creates the last segment).
  mkdir(FAT, 0755);
  mkdir(TFS, 0755);
}
void tearDown(void) {}

// cleanup: remove per-test files/links, keep the fixture parent dirs.
static void cleanup(void) {
  unlink(FAT "/exists");
  unlink(FAT "/ro");
  unlink(TFS "/exists");
  unlink(TFS "/ro");
}

// access: F_OK on existing file = 0; non-existent → ENOENT.
void test_access_existing_fok(void) {
  cleanup();
  int fd = open(FAT "/exists", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);

  TEST_ASSERT_EQUAL_INT(0, access(FAT "/exists", F_OK));
  TEST_ASSERT_EQUAL_INT(0, access(FAT "/exists", R_OK | W_OK | X_OK));

  TEST_ASSERT_EQUAL_INT(-1, access(FAT "/nope", F_OK));
  TEST_ASSERT_EQUAL_INT(ENOENT, errno);
}

// access on a read-only file (0444) under root: R_OK=0, W_OK/X_OK also pass
// (root CAP covers them). Verifies mode is forwarded to the kernel (not the
// old stat fallback that ignored mode).
void test_access_ro_mode_checked(void) {
  cleanup();
  int fd = open(FAT "/ro", O_CREAT | O_WRONLY | O_TRUNC, 0444);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);

  TEST_ASSERT_EQUAL_INT(0, access(FAT "/ro", R_OK));
  // root passes W_OK/X_OK via CAP_DAC_OVERRIDE; asserts the syscall honors
  // mode (old impl ignored mode, making R_OK and W_OK indistinguishable).
  TEST_ASSERT_EQUAL_INT(0, access(FAT "/ro", W_OK));
  TEST_ASSERT_EQUAL_INT(0, access(FAT "/ro", X_OK));
}

// Same path on tmpfs (/run): confirm access uses the same inode_permission.
void test_access_tmpfs(void) {
  cleanup();
  int fd = open(TFS "/exists", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);

  TEST_ASSERT_EQUAL_INT(0, access(TFS "/exists", F_OK));
  TEST_ASSERT_EQUAL_INT(-1, access(TFS "/nope", F_OK));
  TEST_ASSERT_EQUAL_INT(ENOENT, errno);
}

// faccessat(AT_FDCWD) ≡ access; AT_FDCWD is the root-equivalent in a kernel
// without per-process CWD.
void test_faccessat_atfdcwd(void) {
  cleanup();
  int fd = open(FAT "/exists", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);

  TEST_ASSERT_EQUAL_INT(0, faccessat(AT_FDCWD, FAT "/exists", F_OK, 0));
  TEST_ASSERT_EQUAL_INT(0, faccessat(AT_FDCWD, FAT "/exists", R_OK | W_OK, 0));
  TEST_ASSERT_EQUAL_INT(-1, faccessat(AT_FDCWD, FAT "/nope", F_OK, 0));
  TEST_ASSERT_EQUAL_INT(ENOENT, errno);
}

// faccessat on an open dir's dirfd itself (AT_EMPTY_PATH): dirfd points at
// the fixture parent dir; F_OK/R_OK/X_OK must be 0.
void test_faccessat_dirfd_empty_path(void) {
  cleanup();
  int dfd = open(FAT, O_RDONLY | O_DIRECTORY);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, dfd);
  TEST_ASSERT_EQUAL_INT(
      0, faccessat(dfd, "", F_OK | R_OK | W_OK | X_OK, AT_EMPTY_PATH));
  close(dfd);
}

// Invalid mode bits (e.g. 0x100) → EINVAL; invalid flags → EINVAL (Q6 strict).
void test_faccessat_invalid_args(void) {
  cleanup();
  int fd = open(FAT "/exists", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);

  TEST_ASSERT_EQUAL_INT(-1, faccessat(AT_FDCWD, FAT "/exists", 0x100, 0));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
  TEST_ASSERT_EQUAL_INT(-1, faccessat(AT_FDCWD, FAT "/exists", F_OK, 0x400000));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_access_existing_fok);
  RUN_TEST(test_access_ro_mode_checked);
  RUN_TEST(test_access_tmpfs);
  RUN_TEST(test_faccessat_atfdcwd);
  RUN_TEST(test_faccessat_dirfd_empty_path);
  RUN_TEST(test_faccessat_invalid_args);
  return UNITY_END();
}

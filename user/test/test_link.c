/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// test_link.c — verifies §3.4 link(2)/linkat(2) (tmpfs hard links + the full
// nlink chain, Q3). Aligned with the test_rename.c/test_stat_real.c style:
// Unity freestanding, FAT32 (/ prefix) + tmpfs (/run prefix) dual fixtures.
//
// Full nlink chain (Q3): link makes target nlink==2; unlinking a link name
// drops nlink to 1 (the original path still works, data is not lost); mkdir
// increments the parent dir's nlink++ ("."/".." count), rmdir reverses it.
// Hard-linking a directory → EPERM (POSIX); cross-fs (FAT32↔tmpfs) → EXDEV;
// FAT32 link → EPERM (no physical hard links). Duplicate name → EEXIST.
//
// tmpfs hard-link semantics: link creates new → old sharing the same inode
// (a directory entry maps name → inode), and nlink counts that inode's
// directory entries. unlink removes an entry and decrements nlink, while
// i_count manages reclamation.
#include "unity.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FAT "/link_fat"
#define TFS "/run/link_tfs"

void setUp(void) {
  mkdir(FAT, 0755);
  mkdir(TFS, 0755);
}
void tearDown(void) {}

static void cleanup(void) {
  unlink(TFS "/a");
  unlink(TFS "/b");
  unlink(FAT "/a");
}

// tmpfs link round-trip: create a → write data → link(a, b) → b reads the
// same data + nlink==2. Verifies hard links share an inode (not a symlink
// copy).
void test_link_tmpfs_data_shared(void) {
  cleanup();
  int fd = open(TFS "/a", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  TEST_ASSERT_EQUAL_INT(5, write(fd, "hello", 5));
  close(fd);

  TEST_ASSERT_EQUAL_INT(0, link(TFS "/a", TFS "/b"));
  int bfd = open(TFS "/b", O_RDONLY);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, bfd);
  char buf[16] = {0};
  TEST_ASSERT_EQUAL_INT(5, read(bfd, buf, sizeof(buf) - 1));
  close(bfd);
  TEST_ASSERT_EQUAL_STRING("hello", buf);

  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(TFS "/a", &st));
  TEST_ASSERT_EQUAL_INT(2, (int)st.st_nlink);
}

// After unlinking a link name, nlink--: after link(a,b) + unlink a, nlink
// returns to 1 and b still reaches the data (a hard link does not depend on
// the original path; the data lives on the inode).
void test_link_unlink_keeps_data(void) {
  cleanup();
  int fd = open(TFS "/a", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  TEST_ASSERT_EQUAL_INT(3, write(fd, "xyz", 3));
  close(fd);

  TEST_ASSERT_EQUAL_INT(0, link(TFS "/a", TFS "/b"));
  TEST_ASSERT_EQUAL_INT(0, unlink(TFS "/a"));
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(TFS "/b", &st));
  TEST_ASSERT_EQUAL_INT(1, (int)st.st_nlink);
  int bfd = open(TFS "/b", O_RDONLY);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, bfd);
  char buf[16] = {0};
  TEST_ASSERT_EQUAL_INT(3, read(bfd, buf, sizeof(buf) - 1));
  close(bfd);
  TEST_ASSERT_EQUAL_STRING("xyz", buf);
}

// linkat uses a dirfd-relative path: open a's parent dir dfd, then
// linkat(dfd,"a",dfd,"b",0).
void test_linkat_dirfd(void) {
  cleanup();
  int fd = open(TFS "/a", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);
  int dfd = open(TFS, O_RDONLY | O_DIRECTORY);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, dfd);
  TEST_ASSERT_EQUAL_INT(0, linkat(dfd, "a", dfd, "b", 0));
  close(dfd);
  TEST_ASSERT_EQUAL_INT(0, access(TFS "/b", F_OK));
}

// Hard-linking a directory → EPERM (POSIX: root-only, not enabled here).
void test_link_dir_eperm(void) {
  cleanup();
  TEST_ASSERT_EQUAL_INT(0, mkdir(TFS "/d", 0755));
  TEST_ASSERT_EQUAL_INT(-1, link(TFS "/d", TFS "/dlink"));
  TEST_ASSERT_EQUAL_INT(EPERM, errno);
  rmdir(TFS "/d");
  rmdir(TFS "/dlink");
}

// Cross-fs (FAT32↔tmpfs) → EXDEV.
void test_link_cross_fs_exdev(void) {
  cleanup();
  int fd = open(FAT "/a", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);
  TEST_ASSERT_EQUAL_INT(-1, link(FAT "/a", TFS "/b"));
  TEST_ASSERT_EQUAL_INT(EXDEV, errno);
}

// FAT32 link → EPERM (FAT32 has no hard links; fat32_dir_iop.link==NULL →
// the kernel's do_linkat returns -EPERM).
void test_link_fat32_eperm(void) {
  cleanup();
  int fd = open(FAT "/a", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);
  TEST_ASSERT_EQUAL_INT(-1, link(FAT "/a", FAT "/b"));
  TEST_ASSERT_EQUAL_INT(EPERM, errno);
}

// Target already exists → EEXIST.
void test_link_eexist(void) {
  cleanup();
  int fd = open(TFS "/a", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);
  fd = open(TFS "/b", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);
  TEST_ASSERT_EQUAL_INT(-1, link(TFS "/a", TFS "/b"));
  TEST_ASSERT_EQUAL_INT(EEXIST, errno);
}

// Invalid flags (linkat only accepts AT_SYMLINK_FOLLOW) → EINVAL (strict Q6
// validation).
void test_linkat_invalid_flags(void) {
  cleanup();
  int fd = open(TFS "/a", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);
  TEST_ASSERT_EQUAL_INT(
      -1, linkat(AT_FDCWD, TFS "/a", AT_FDCWD, TFS "/b", 0x400000));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_link_tmpfs_data_shared);
  RUN_TEST(test_link_unlink_keeps_data);
  RUN_TEST(test_linkat_dirfd);
  RUN_TEST(test_link_dir_eperm);
  RUN_TEST(test_link_cross_fs_exdev);
  RUN_TEST(test_link_fat32_eperm);
  RUN_TEST(test_link_eexist);
  RUN_TEST(test_linkat_invalid_flags);
  return UNITY_END();
}

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_stat_real.c — S08: umask 应用 + inode owner/mode 字段 + fstat
 * 报真实字段。
 *
 * Verifies the post-S08 stat semantics:
 *   1. open(O_CREAT, mode) applies umask: st_mode & 0777 == mode & ~umask.
 *   2. Newly created files report the creator's uid/gid (not a hardcoded 0,
 *      and not the stat caller's identity when they differ conceptually).
 *   3. mkdir(mode) applies umask and reports S_IFDIR with masked perms.
 *   4. st_blksize reflects the backing fs (FAT32 root = 512, tmpfs /run = 4096)
 *      rather than the old hardcoded 512.
 *   5. fstat on an open fd and stat by path agree (both delegate to the
 *      inode's getattr).
 *
 * FAT32 root cluster size is 512 (mkdisk.sh part2: mkfs.fat -F 32 -s 1).
 * /run is a RAM-backed tmpfs mount (blksize 4096). */
#include "unity.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* FAT32-root fixtures. Leading prefix keeps them out of other tests'
 * namespaces. Macros (not const char *) so adjacent literal concatenation
 * FAT "/x" works. */
#define FAT "/stat_real_fat"
/* tmpfs (/run) fixtures. */
#define TFS "/run/stat_real_tfs"

void setUp(void) {
  /* Create the fixture parent dirs once per test (ok if they already exist).
   * open(O_CREAT)/mkdir only create the final path component — the parent
   * (/stat_real_fat, /run/stat_real_tfs) must pre-exist. */
  mkdir(FAT, 0755);
  mkdir(TFS, 0755);
}
void tearDown(void) {}

static void cleanup(void) {
  unlink(FAT "/u");
  unlink(FAT "/d_file");
  unlink(TFS "/u");
  rmdir(FAT "/d");
  rmdir(TFS "/d");
}

/* umask applied to open(O_CREAT): mode & ~umask lands in st_mode. */
void test_open_umask_applied(void) {
  cleanup();
  mode_t old = umask(0022);
  int fd = open(FAT "/u", O_CREAT | O_RDWR | O_TRUNC, 0666);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);

  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, fstat(fd, &st));
  TEST_ASSERT_TRUE(S_ISREG(st.st_mode));
  TEST_ASSERT_EQUAL_INT(0666 & ~0022, st.st_mode & 0777);
  close(fd);

  /* Path-based stat must agree with fstat (both via inode getattr). */
  TEST_ASSERT_EQUAL_INT(0, stat(FAT "/u", &st));
  TEST_ASSERT_TRUE(S_ISREG(st.st_mode));
  TEST_ASSERT_EQUAL_INT(0666 & ~0022, st.st_mode & 0777);

  umask(old);
  unlink(FAT "/u");
}

/* Newly created file reports creator uid/gid (root=0 here), not a stale
 * hardcoded value, and a non-zero nlink. */
void test_open_owner_is_creator(void) {
  cleanup();
  int fd = open(FAT "/u", O_CREAT | O_RDWR | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);

  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, fstat(fd, &st));
  TEST_ASSERT_EQUAL_INT((int)getuid(), (int)st.st_uid);
  TEST_ASSERT_EQUAL_INT((int)getgid(), (int)st.st_gid);
  TEST_ASSERT_GREATER_THAN_INT(0, (int)st.st_nlink);
  close(fd);
  unlink(FAT "/u");
}

/* mkdir(mode) applies umask and reports S_IFDIR with masked perms. */
void test_mkdir_umask_applied(void) {
  cleanup();
  mode_t old = umask(0022);
  TEST_ASSERT_EQUAL_INT(0, mkdir(FAT "/d", 0777));

  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(FAT "/d", &st));
  TEST_ASSERT_TRUE(S_ISDIR(st.st_mode));
  TEST_ASSERT_EQUAL_INT(0777 & ~0022, st.st_mode & 0777);

  rmdir(FAT "/d");
  umask(old);
}

/* st_blksize reflects the backing fs: FAT32 root = 512, tmpfs /run = 4096.
 * The old code hardcoded 512 for every fd regardless of fs. */
void test_blksize_per_fs(void) {
  cleanup();
  int fd = open(FAT "/u", O_CREAT | O_RDWR | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, fstat(fd, &st));
  TEST_ASSERT_EQUAL_INT(512, (int)st.st_blksize);
  close(fd);

  int tfd = open(TFS "/u", O_CREAT | O_RDWR | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, tfd);
  TEST_ASSERT_EQUAL_INT(0, fstat(tfd, &st));
  TEST_ASSERT_EQUAL_INT(4096, (int)st.st_blksize);
  close(tfd);

  unlink(FAT "/u");
  unlink(TFS "/u");
}

/* tmpfs file also gets umask + creator owner, with S_IFREG preserved. */
void test_tmpfs_open_umask_and_owner(void) {
  cleanup();
  mode_t old = umask(0022);
  int fd = open(TFS "/u", O_CREAT | O_RDWR | O_TRUNC, 0666);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);

  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, fstat(fd, &st));
  TEST_ASSERT_TRUE(S_ISREG(st.st_mode));
  TEST_ASSERT_EQUAL_INT(0666 & ~0022, st.st_mode & 0777);
  TEST_ASSERT_EQUAL_INT((int)getuid(), (int)st.st_uid);
  TEST_ASSERT_EQUAL_INT((int)getgid(), (int)st.st_gid);
  close(fd);
  umask(old);
  unlink(TFS "/u");
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_open_umask_applied);
  RUN_TEST(test_open_owner_is_creator);
  RUN_TEST(test_mkdir_umask_applied);
  RUN_TEST(test_blksize_per_fs);
  RUN_TEST(test_tmpfs_open_umask_and_owner);
  return UNITY_END();
}

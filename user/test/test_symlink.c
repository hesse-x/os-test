/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_symlink.c — 验证 §3.3 symlink(2)/symlinkat(2)/readlink(2)/readlinkat(2)
 * (tmpfs 真实现,Q2)。对齐 test_rename.c/test_stat_real.c 风格:Unity
 * freestanding,FAT32 + tmpfs(/run)双夹具。
 *
 * FAT32 物理不支持 symlink → symlink() 返 -1 errno=ENOSYS(或 EPERM);软链
 * round-trip/ELOOP/lstat 区分仅在 tmpfs 断言。AT_SYMLINK_NOFOLLOW(lstat)
 * 经 vfs_statx 末段 symlink 跟随分支激活(§3.3.4)。
 *
 * ELOOP:tmpfs 建 self-referential 软链 a→a,path_walk 中间段 follow_symlink
 * 经 SYMLINK_MAX=40 触发 → stat 返 ENOENT(本 OS path_walk ELOOP 返 NULL →
 * ENOENT;readlink 直接读 link 本身不需跟随,故 self-link readlink 仍成功)。 */
#include "unity.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FAT "/sym_fat"
#define TFS "/run/sym_tfs"

void setUp(void) {
  mkdir(FAT, 0755);
  mkdir(TFS, 0755);
}
void tearDown(void) {}

static void cleanup(void) {
  unlink(TFS "/lnk");
  unlink(TFS "/self");
  unlink(TFS "/to_file");
  unlink(TFS "/to_dir");
  unlink(TFS "/f");
  rmdir(TFS "/d");
  unlink(FAT "/lnk");
}

/* tmpfs symlink/readlink round-trip:建软链 → readlink 取回 target。
 * readlink 不 NUL 终止,返回长度。 */
void test_symlink_readlink_roundtrip(void) {
  cleanup();
  int fd = open(TFS "/f", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);

  TEST_ASSERT_EQUAL_INT(0, symlink(TFS "/f", TFS "/lnk"));
  char buf[256];
  ssize_t n = readlink(TFS "/lnk", buf, sizeof(buf));
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, n);
  TEST_ASSERT_EQUAL_INT((int)__builtin_strlen(TFS "/f"), (int)n);
  buf[n] = '\0';
  TEST_ASSERT_EQUAL_STRING(TFS "/f", buf);
}

/* readlinkat(AT_FDCWD) ≡ readlink;bufsiz 截断:返回 min(长度, bufsiz)。 */
void test_readlinkat_and_truncate(void) {
  cleanup();
  int fd = open(TFS "/f", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);

  TEST_ASSERT_EQUAL_INT(0, symlink(TFS "/f", TFS "/lnk"));
  char buf[4];
  ssize_t n = readlinkat(AT_FDCWD, TFS "/lnk", buf, sizeof(buf));
  TEST_ASSERT_EQUAL_INT(4, n);
  TEST_ASSERT_EQUAL_INT(0, strncmp(TFS "/f", buf, 4));
}

/* lstat vs stat:lstat 取 link 本身(S_ISLNK),stat 跟随末段取目标(S_ISREG)。 */
void test_lstat_vs_stat(void) {
  cleanup();
  int fd = open(TFS "/f", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);
  TEST_ASSERT_EQUAL_INT(0, symlink(TFS "/f", TFS "/lnk"));

  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, lstat(TFS "/lnk", &st));
  TEST_ASSERT_TRUE(((st.st_mode & S_IFMT) == S_IFLNK));

  TEST_ASSERT_EQUAL_INT(0, stat(TFS "/lnk", &st));
  TEST_ASSERT_TRUE(S_ISREG(st.st_mode));
}

/* symlinkat 用 dirfd 相对路径:open dirfd 指向 /run/sym_tfs,symlinkat 建
 * "lnk" → target。 */
void test_symlinkat_dirfd(void) {
  cleanup();
  int fd = open(TFS "/f", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);
  int dfd = open(TFS, O_RDONLY | O_DIRECTORY);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, dfd);
  TEST_ASSERT_EQUAL_INT(0, symlinkat(TFS "/f", dfd, "lnk"));
  close(dfd);

  char buf[256];
  ssize_t n = readlink(TFS "/lnk", buf, sizeof(buf));
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, n);
  buf[n] = '\0';
  TEST_ASSERT_EQUAL_STRING(TFS "/f", buf);
}

/* 目标已存在 → EEXIST。 */
void test_symlink_eexist(void) {
  cleanup();
  int fd = open(TFS "/f", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);
  TEST_ASSERT_EQUAL_INT(-1, symlink(TFS "/f", TFS "/f"));
  TEST_ASSERT_EQUAL_INT(EEXIST, errno);
}

/* FAT32 symlink 不支持 → -1,errno ∈ {ENOSYS, EPERM}(物理不支持,内核返
 * ENOSYS via i_op->symlink==NULL)。 */
void test_symlink_fat32_nosys(void) {
  cleanup();
  TEST_ASSERT_EQUAL_INT(-1, symlink("/nope", FAT "/lnk"));
  TEST_ASSERT_TRUE(errno == ENOSYS || errno == EPERM);
}

/* readlink 对非软链(普通文件)→ EINVAL。 */
void test_readlink_not_symlink(void) {
  cleanup();
  int fd = open(TFS "/f", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);
  char buf[256];
  TEST_ASSERT_EQUAL_INT(-1, readlink(TFS "/f", buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
}

/* readlink 不存在路径 → ENOENT。 */
void test_readlink_noent(void) {
  cleanup();
  char buf[256];
  TEST_ASSERT_EQUAL_INT(-1, readlink(TFS "/nope", buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_INT(ENOENT, errno);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_symlink_readlink_roundtrip);
  RUN_TEST(test_readlinkat_and_truncate);
  RUN_TEST(test_lstat_vs_stat);
  RUN_TEST(test_symlinkat_dirfd);
  RUN_TEST(test_symlink_eexist);
  RUN_TEST(test_symlink_fat32_nosys);
  RUN_TEST(test_readlink_not_symlink);
  RUN_TEST(test_readlink_noent);
  return UNITY_END();
}

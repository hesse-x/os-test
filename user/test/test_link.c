/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_link.c — 验证 §3.4 link(2)/linkat(2)(tmpfs 硬链接 + nlink 全链路,
 * Q3)。对齐 test_rename.c/test_stat_real.c 风格:Unity freestanding,FAT32
 * (/ 前缀)+ tmpfs(/run 前缀)双夹具。
 *
 * nlink 全链路(Q3):link 使 target nlink==2;unlink 链名后 nlink==1
 * (原路径仍可达,数据不丢);mkdir 使父目录 nlink++(目录的 "."/".." 计数),
 * rmdir 反向。硬链目录 → EPERM(POSIX);跨 fs(FAT32↔tmpfs)→ EXDEV;
 * FAT32 link → EPERM(物理无硬链)。重名 → EEXIST。
 *
 * tmpfs 硬链语义:link 建 new → old 共享同一 inode(目录项是名字→inode 映射),
 * nlink 是该 inode 的目录项计数。unlink 摘目录项 nlink--,i_count 管回收。 */
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

/* tmpfs link round-trip:建 a → write 数据 → link(a, b) → b 可读同数据 +
 * nlink==2。验证硬链共享 inode(非软链的拷贝)。 */
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

/* unlink 链名后 nlink--:link(a,b) 后 unlink a,nlink 回 1,且 b 仍可达数据
 * (硬链不依赖原路径,数据在 inode 上)。 */
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

/* linkat 用 dirfd 相对路径:open a 的父目录 dfd,linkat(dfd,"a",dfd,"b",0)。 */
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

/* 硬链目录 → EPERM(POSIX:仅 root 可,本 OS 不开)。 */
void test_link_dir_eperm(void) {
  cleanup();
  TEST_ASSERT_EQUAL_INT(0, mkdir(TFS "/d", 0755));
  TEST_ASSERT_EQUAL_INT(-1, link(TFS "/d", TFS "/dlink"));
  TEST_ASSERT_EQUAL_INT(EPERM, errno);
  rmdir(TFS "/d");
  rmdir(TFS "/dlink");
}

/* 跨 fs(FAT32↔tmpfs)→ EXDEV。 */
void test_link_cross_fs_exdev(void) {
  cleanup();
  int fd = open(FAT "/a", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);
  TEST_ASSERT_EQUAL_INT(-1, link(FAT "/a", TFS "/b"));
  TEST_ASSERT_EQUAL_INT(EXDEV, errno);
}

/* FAT32 link → EPERM(FAT32 物理无硬链,fat32_dir_iop.link==NULL → 内核
 * do_linkat 返 -EPERM)。 */
void test_link_fat32_eperm(void) {
  cleanup();
  int fd = open(FAT "/a", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);
  TEST_ASSERT_EQUAL_INT(-1, link(FAT "/a", FAT "/b"));
  TEST_ASSERT_EQUAL_INT(EPERM, errno);
}

/* 目标已存在 → EEXIST。 */
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

/* 非法 flags(linkat 仅 AT_SYMLINK_FOLLOW 合法)→ EINVAL(Q6 严格校验)。 */
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

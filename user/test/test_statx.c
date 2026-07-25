/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_statx.c — SYS_STATX(332) 直连 + stat/lstat/fstat/fstatat 收敛验证。
 *
 * 内核只暴露 statx 一个元数据 syscall；libc 的 stat/lstat/fstat/fstatat 全
 * 部经 statx 实现并做 statx→stat 缩窄转换。本测试交叉验证：
 *   1. statx 基本字段（mask/mode/ino/size/nlink/blksize/uid/gid/rdev）与
 *      stat() 结果一致（转换无损）。
 *   2. AT_EMPTY_PATH + 空路径 ≡ fstat；dirfd + 相对路径解析正确。
 *   3. lstat/fstatat 与 stat 交叉一致。
 *   4. 无 inode 的 fd 类型（pipe）经 statx 报 S_IFIFO。
 *   5. devtmpfs 设备节点 rdev major/minor 重组 == legacy st_rdev。
 *   6. 错误路径：ENOENT / EBADF / EINVAL（SYNC 双置位、未知 flag 位）。 */
#include "unity.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

/* FAT32-root fixtures（簇 512B，同 test_stat_real）。 */
#define FAT "/statx_fat"

void setUp(void) { mkdir(FAT, 0755); }
void tearDown(void) {}

static void cleanup(void) { unlink(FAT "/u"); }

static int make_file(const char *path, const char *data, int len) {
  int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
  if (fd < 0)
    return -1;
  if (write(fd, data, len) != len) {
    close(fd);
    return -1;
  }
  close(fd);
  return 0;
}

/* statx 基本字段与 stat() 一致；掩码恰为 STATX_BASIC_STATS。 */
void test_statx_basic_fields(void) {
  cleanup();
  umask(0);
  TEST_ASSERT_EQUAL_INT(0, make_file(FAT "/u", "hello", 5));

  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(FAT "/u", &st));

  struct statx sx;
  memset(&sx, 0xA5, sizeof(sx)); /* 毒药填充：验证内核写全字段 */
  TEST_ASSERT_EQUAL_INT(0,
                        statx(AT_FDCWD, FAT "/u", 0, STATX_BASIC_STATS, &sx));

  TEST_ASSERT_EQUAL_HEX32(STATX_BASIC_STATS, sx.stx_mask);
  TEST_ASSERT_TRUE((sx.stx_mode & S_IFMT) == S_IFREG);
  TEST_ASSERT_EQUAL_INT(0644, sx.stx_mode & 0777);
  TEST_ASSERT_EQUAL_INT(st.st_mode, sx.stx_mode);
  TEST_ASSERT_EQUAL_INT(st.st_uid, sx.stx_uid);
  TEST_ASSERT_EQUAL_INT(st.st_gid, sx.stx_gid);
  TEST_ASSERT_EQUAL_INT(st.st_nlink, sx.stx_nlink);
  TEST_ASSERT_EQUAL_UINT64(st.st_ino, sx.stx_ino);
  TEST_ASSERT_EQUAL_UINT64(5, sx.stx_size);
  TEST_ASSERT_EQUAL_INT(512, sx.stx_blksize); /* FAT32 root 簇大小 */
  TEST_ASSERT_EQUAL_INT(st.st_blksize, sx.stx_blksize);
  TEST_ASSERT_EQUAL_UINT64(st.st_blocks, sx.stx_blocks);
  /* major/minor 重组 == legacy dev_t 编码 */
  TEST_ASSERT_EQUAL_UINT64(st.st_rdev,
                           makedev(sx.stx_rdev_major, sx.stx_rdev_minor));
  TEST_ASSERT_EQUAL_UINT64(st.st_dev,
                           makedev(sx.stx_dev_major, sx.stx_dev_minor));

  unlink(FAT "/u");
}

/* AT_EMPTY_PATH + 空路径 ≡ fstat（同 fd 同结果）。 */
void test_statx_empty_path_matches_fstat(void) {
  cleanup();
  int fd = open(FAT "/u", O_CREAT | O_RDWR | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);

  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, fstat(fd, &st));

  struct statx sx;
  TEST_ASSERT_EQUAL_INT(0,
                        statx(fd, "", AT_EMPTY_PATH, STATX_BASIC_STATS, &sx));
  TEST_ASSERT_EQUAL_HEX32(STATX_BASIC_STATS, sx.stx_mask);
  TEST_ASSERT_EQUAL_UINT64(st.st_ino, sx.stx_ino);
  TEST_ASSERT_EQUAL_INT(st.st_mode, sx.stx_mode);
  TEST_ASSERT_EQUAL_UINT64(st.st_size, sx.stx_size);

  close(fd);
  unlink(FAT "/u");
}

/* lstat（经 statx AT_SYMLINK_NOFOLLOW）与 stat 一致（本 OS 无 symlink）。 */
void test_lstat_matches_stat(void) {
  cleanup();
  TEST_ASSERT_EQUAL_INT(0, make_file(FAT "/u", "x", 1));

  struct stat a, b;
  TEST_ASSERT_EQUAL_INT(0, stat(FAT "/u", &a));
  TEST_ASSERT_EQUAL_INT(0, lstat(FAT "/u", &b));
  TEST_ASSERT_TRUE(S_ISREG(b.st_mode));
  TEST_ASSERT_EQUAL_UINT64(a.st_ino, b.st_ino);
  TEST_ASSERT_EQUAL_INT(a.st_mode, b.st_mode);

  unlink(FAT "/u");
}

/* dirfd + 相对路径：statx 与 fstatat 都解析到同一 inode。 */
void test_statx_dirfd_relative(void) {
  cleanup();
  TEST_ASSERT_EQUAL_INT(0, make_file(FAT "/u", "abc", 3));

  int dfd = open(FAT, O_RDONLY | O_DIRECTORY);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, dfd);

  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(FAT "/u", &st));

  struct statx sx;
  TEST_ASSERT_EQUAL_INT(0, statx(dfd, "u", 0, STATX_BASIC_STATS, &sx));
  TEST_ASSERT_EQUAL_UINT64(st.st_ino, sx.stx_ino);
  TEST_ASSERT_EQUAL_UINT64(3, sx.stx_size);

  /* fstatat（libc 亦走 statx）同路径一致 */
  struct stat st2;
  TEST_ASSERT_EQUAL_INT(0, fstatat(dfd, "u", &st2, 0));
  TEST_ASSERT_EQUAL_UINT64(st.st_ino, st2.st_ino);
  TEST_ASSERT_EQUAL_INT(st.st_mode, st2.st_mode);

  close(dfd);
  unlink(FAT "/u");
}

/* 无 inode 的 fd 类型：pipe 经 statx 报 S_IFIFO。 */
void test_statx_pipe_fd(void) {
  int p[2];
  TEST_ASSERT_EQUAL_INT(0, pipe(p));

  struct statx sx;
  TEST_ASSERT_EQUAL_INT(0,
                        statx(p[0], "", AT_EMPTY_PATH, STATX_BASIC_STATS, &sx));
  TEST_ASSERT_TRUE((sx.stx_mode & S_IFMT) == S_IFIFO);

  close(p[0]);
  close(p[1]);
}

/* devtmpfs 设备节点：statx rdev major/minor 重组 == legacy st_rdev。 */
void test_statx_dev_rdev(void) {
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat("/dev/sda", &st));
  TEST_ASSERT_TRUE(S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode));

  struct statx sx;
  TEST_ASSERT_EQUAL_INT(0,
                        statx(AT_FDCWD, "/dev/sda", 0, STATX_BASIC_STATS, &sx));
  TEST_ASSERT_EQUAL_UINT64(st.st_rdev,
                           makedev(sx.stx_rdev_major, sx.stx_rdev_minor));
  TEST_ASSERT_EQUAL_INT(st.st_mode, sx.stx_mode);
}

/* 目录：statx("/") 报 S_IFDIR。 */
void test_statx_directory(void) {
  struct statx sx;
  TEST_ASSERT_EQUAL_INT(0, statx(AT_FDCWD, "/", 0, STATX_BASIC_STATS, &sx));
  TEST_ASSERT_TRUE((sx.stx_mode & S_IFMT) == S_IFDIR);
}

/* 错误路径：ENOENT / EBADF / EINVAL。 */
void test_statx_errors(void) {
  struct statx sx;

  /* 不存在路径 → ENOENT */
  TEST_ASSERT_EQUAL_INT(
      -1, statx(AT_FDCWD, FAT "/nonexistent", 0, STATX_BASIC_STATS, &sx));
  TEST_ASSERT_EQUAL_INT(ENOENT, errno);

  /* 坏 fd + AT_EMPTY_PATH → EBADF */
  TEST_ASSERT_EQUAL_INT(-1,
                        statx(9999, "", AT_EMPTY_PATH, STATX_BASIC_STATS, &sx));
  TEST_ASSERT_EQUAL_INT(EBADF, errno);

  /* FORCE_SYNC|DONT_SYNC 同置 → EINVAL（Linux do_statx） */
  TEST_ASSERT_EQUAL_INT(-1, statx(AT_FDCWD, "/",
                                  AT_STATX_FORCE_SYNC | AT_STATX_DONT_SYNC,
                                  STATX_BASIC_STATS, &sx));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);

  /* 未知 flag 位 → EINVAL */
  TEST_ASSERT_EQUAL_INT(-1, statx(AT_FDCWD, "/", 0x20, STATX_BASIC_STATS, &sx));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);

  /* 空路径无 AT_EMPTY_PATH → ENOENT */
  TEST_ASSERT_EQUAL_INT(-1, statx(AT_FDCWD, "", 0, STATX_BASIC_STATS, &sx));
  TEST_ASSERT_EQUAL_INT(ENOENT, errno);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_statx_basic_fields);
  RUN_TEST(test_statx_empty_path_matches_fstat);
  RUN_TEST(test_lstat_matches_stat);
  RUN_TEST(test_statx_dirfd_relative);
  RUN_TEST(test_statx_pipe_fd);
  RUN_TEST(test_statx_dev_rdev);
  RUN_TEST(test_statx_directory);
  RUN_TEST(test_statx_errors);
  return UNITY_END();
}

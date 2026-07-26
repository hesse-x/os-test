/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_link_utimensat.c — 验证 §3.5 utimensat(2)(inode 内存时间戳,Q5)。
 * 对齐 test_rename.c/test_stat_real.c 风格:Unity freestanding,FAT32(/ 前缀)+
 * tmpfs(/run 前缀)双夹具。
 *
 * 时间戳内存态:inode atime/mtime/ctime(ns),getattr 读 st_mtim/st_atim。
 * 不落盘 FAT32(Q5:llvm libc utimensat test 不跨重启)。UTIME_NOW/OMIT 见
 * uapi fcntl.h。link/nlink 断言(Q3)在阶段 3 通过 #ifdef TEST_LINK 启用,
 * 阶段 1 仅 utimensat。
 *
 * FAT32 vs tmpfs 差异:fat32 inode 无 fs 内部强引用,utimensat 释放 inode 后
 * 可被回收,再 stat 经 lookup 建新 inode → mtime 回 fat32_iget 初始化的当前
 * 时间(非显式值);tmpfs 子项持 inode 引用不回收 → utimensat 显式值精确往返。
 * 故显式值往返仅在 tmpfs 断言;fat32 断言 mtime>0(UTIME_NOW/NULL)或接受回收
 * 语义。 */
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

/* tmpfs:utimensat 显式值精确往返(inode 不回收,ns=0 拆 sec 后无损)。 */
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

/* UTIME_NOW:tv_nsec=UTIME_NOW → 设为当前时间。fat32 与 tmpfs 均应返非零
 * (fat32 即便 inode 回收,新建亦初始化为当前时间;tmpfs 直接写 now)。 */
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

/* UTIME_OMIT(tmpfs):先设显式 1700000000,再用 UTIME_OMIT 调用,验证 mtime
 * 保持 1700000000(UTIME_OMIT 不覆盖该字段)。 */
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

/* times=NULL → atime=mtime=now(对齐 Linux;需写权限)。root 放行 W_OK。
 * fat32 与 tmpfs 均应返非零 mtime。 */
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

/* atime 也应被设:UTIME_NOW 设 atime 后 stat 读 st_atim > 0。 */
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

/* 非法 flags(非 AT_SYMLINK_NOFOLLOW)→ EINVAL;tv_nsec 越界(>=1e9 或 <0,
 * 非 NOW/OMIT)→ EINVAL(Q6 严格校验)。 */
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

/* 路径不存在 → ENOENT。 */
void test_utimensat_noent(void) {
  cleanup();
  TEST_ASSERT_EQUAL_INT(-1, utimensat(AT_FDCWD, FAT "/nope", NULL, 0));
  TEST_ASSERT_EQUAL_INT(ENOENT, errno);
}

/* ===================== link / nlink (阶段 3, TEST_LINK) =====================
 */
#ifdef TEST_LINK
/* 阶段 3 启用:tmpfs 硬链 nlink==2;unlink 后 nlink==1;硬链目录 EPERM;
 * 跨 fs(FAT32↔tmpfs)EXDEV;FAT32 link EPERM。此处仅占位,阶段 3 填充。 */
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

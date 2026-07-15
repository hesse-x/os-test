/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_rename.c — 验证 §3.1 SYS_RENAME(tmpfs rename 原子写基座)。
 * Unity freestanding:setUp/tearDown 空实现;断言 TEST_ASSERT_*。
 * 参考 test_libudev 模板(同构于其它 vfs 测试)。 */
#include "unity.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* 测试在 /run(RAM tmpfs) 下工作,对齐 db 实际落点目录语义。 */
#define RENAME_DIR "/run/udev/data"

void setUp(void) {}
void tearDown(void) {}

static int write_file(const char *path, const char *content) {
  int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0)
    return -1;
  ssize_t n = write(fd, content, strlen(content));
  close(fd);
  return (n > 0) ? 0 : -1;
}

static int read_file(const char *path, char *buf, size_t cap) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  ssize_t n = read(fd, buf, cap - 1);
  close(fd);
  if (n < 0)
    return -1;
  buf[n] = '\0';
  return 0;
}

/* 同目录 rename(基本):db 场景核心。 */
void test_rename_same_dir(void) {
  const char *oldp = RENAME_DIR "/rename_basic_old";
  const char *newp = RENAME_DIR "/rename_basic_new";
  write_file(oldp, "hello");
  TEST_ASSERT_EQUAL_INT(0, rename(oldp, newp));
  char buf[64];
  TEST_ASSERT_EQUAL_INT(0, read_file(newp, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("hello", buf);
  /* 旧路径应 ENOENT */
  int fd = open(oldp, O_RDONLY);
  if (fd >= 0)
    close(fd);
  TEST_ASSERT_LESS_THAN_INT(0, fd);
  unlink(newp);
}

/* rename 覆盖已存在目标:db 原子更新覆盖语义。 */
void test_rename_overwrite(void) {
  const char *oldp = RENAME_DIR "/rename_overwrite_old";
  const char *newp = RENAME_DIR "/rename_overwrite_new";
  write_file(newp, "old");
  write_file(oldp, "new");
  TEST_ASSERT_EQUAL_INT(0, rename(oldp, newp));
  char buf[64];
  TEST_ASSERT_EQUAL_INT(0, read_file(newp, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("new", buf);
  unlink(newp);
}

/* rename old 不存在 → -1/ENOENT:错误路径。 */
void test_rename_old_missing(void) {
  const char *oldp = RENAME_DIR "/rename_nope_missing";
  const char *newp = RENAME_DIR "/rename_nope_new";
  TEST_ASSERT_EQUAL_INT(-1, rename(oldp, newp));
}

/* rename 后旧路径 ENOENT + 新路径可读:原子切换验证。 */
void test_rename_atomicity(void) {
  const char *oldp = RENAME_DIR "/rename_atom_old";
  const char *newp = RENAME_DIR "/rename_atom_new";
  write_file(oldp, "atomic");
  TEST_ASSERT_EQUAL_INT(0, rename(oldp, newp));
  char buf[64];
  TEST_ASSERT_EQUAL_INT(0, read_file(newp, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("atomic", buf);
  int fd = open(oldp, O_RDONLY);
  if (fd >= 0)
    close(fd);
  TEST_ASSERT_LESS_THAN_INT(0, fd);
  unlink(newp);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_rename_same_dir);
  RUN_TEST(test_rename_overwrite);
  RUN_TEST(test_rename_old_missing);
  RUN_TEST(test_rename_atomicity);
  return UNITY_END();
}

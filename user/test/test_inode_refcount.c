/*
 * Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unity.h>
#include <xos/errno.h>

void setUp(void) {}
void tearDown(void) {}

/* 1. 多次开关同一文件,path_walk 中间段 inode_put 配对不泄漏(hash 表不膨胀)。 */
void test_inode_refcount_repeat_open_close(void) {
  for (int i = 0; i < 50; i++) {
    int fd = open("/vfs_test_rc", O_CREAT | O_RDWR, 0644);
    TEST_ASSERT_TRUE(fd >= 0);
    if (fd >= 0)
      close(fd);
  }
  /* 若 path_walk 中间段泄漏,多次后 hash 表/内存耗尽导致 open 失败。 */
  int fd = open("/vfs_test_rc", O_RDONLY);
  TEST_ASSERT_TRUE(fd >= 0);
  if (fd >= 0)
    close(fd);
  unlink("/vfs_test_rc");
}

/* 2. 深层路径多次开关(每段 path_walk lookup+put 配对)。 */
void test_inode_refcount_deep_path(void) {
  mkdir("/vfs_rc_d", 0755);
  for (int i = 0; i < 20; i++) {
    int fd = open("/vfs_rc_d/f", O_CREAT | O_RDWR, 0644);
    TEST_ASSERT_TRUE(fd >= 0);
    if (fd >= 0)
      close(fd);
  }
  unlink("/vfs_rc_d/f");
  rmdir("/vfs_rc_d");
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();
  RUN_TEST(test_inode_refcount_repeat_open_close);
  RUN_TEST(test_inode_refcount_deep_path);
  return UNITY_END();
}

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

// 1. Repeated open/close of the same file: path_walk's intermediate inode_put
// pairs must not leak (hash table must not grow).
void test_inode_refcount_repeat_open_close(void) {
  for (int i = 0; i < 50; i++) {
    int fd = open("/vfs_test_rc", O_CREAT | O_RDWR, 0644);
    TEST_ASSERT_TRUE(fd >= 0);
    if (fd >= 0)
      close(fd);
  }
  // If path_walk's intermediate segments leak, repeated runs exhaust the hash
  // table/memory and open fails.
  int fd = open("/vfs_test_rc", O_RDONLY);
  TEST_ASSERT_TRUE(fd >= 0);
  if (fd >= 0)
    close(fd);
  unlink("/vfs_test_rc");
}

// 2. Deep path repeated open/close (each path_walk segment lookup+put paired).
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

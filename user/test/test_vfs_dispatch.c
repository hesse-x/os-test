/*
 * Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unity.h>
#include <xos/errno.h>

void setUp(void) {}
void tearDown(void) {}

/* 1. 只读 fs (sysfs) 上 O_CREAT 应失败(EACCES:parent->i_op->create 为 NULL,对齐
 * Linux vfs_create)。 */
void test_vfs_dispatch_sysfs_create_denied(void) {
  errno = 0;
  int fd = open("/sys/foo_bar_nonexistent", O_CREAT | O_WRONLY, 0644);
  TEST_ASSERT_EQUAL_INT(-1, fd);
  TEST_ASSERT_EQUAL_INT(EACCES, errno);
}

/* 2. 只读 fs 无 setattr:truncate 应失败(EISDIR:/sys 是目录,对齐 Linux truncate
 * 优先判 INODE_REGULAR)。 */
void test_vfs_dispatch_sysfs_truncate_denied(void) {
  errno = 0;
  int rc = truncate("/sys", 0);
  TEST_ASSERT_EQUAL_INT(-1, rc);
  TEST_ASSERT_EQUAL_INT(EISDIR,
                        errno); /* 目录 truncate → EISDIR(非旧的 EINVAL) */
}

/* 3. fat32 上 O_CREAT|O_TRUNC 建文件并截 0(经 fat32_dir_create +
 * fat32_setattr)。 */
void test_vfs_dispatch_fat32_create_trunc(void) {
  int fd = open("/vfs_test_ct", O_CREAT | O_TRUNC | O_WRONLY, 0644);
  TEST_ASSERT_TRUE(fd >= 0);
  if (fd >= 0) {
    struct stat st;
    TEST_ASSERT_EQUAL_INT(0, fstat(fd, &st));
    TEST_ASSERT_EQUAL_INT(0, (int)st.st_size);
    close(fd);
    unlink("/vfs_test_ct");
  }
}

/* 4. fat32 sys_truncate 经 i_op->setattr 改大小。 */
void test_vfs_dispatch_fat32_truncate_grow(void) {
  int fd = open("/vfs_test_tr", O_CREAT | O_WRONLY, 0644);
  if (fd >= 0) {
    close(fd);
    int rc = truncate("/vfs_test_tr", 100);
    TEST_ASSERT_EQUAL_INT(0, rc);
    struct stat st;
    TEST_ASSERT_EQUAL_INT(0, stat("/vfs_test_tr", &st));
    TEST_ASSERT_EQUAL_INT(100, (int)st.st_size);
    unlink("/vfs_test_tr");
  } else {
    TEST_ASSERT_TRUE(1);
  }
}

/* 5. fat32 sys_ftruncate 经 f->inode->i_op->setattr。 */
void test_vfs_dispatch_fat32_ftruncate(void) {
  int fd = open("/vfs_test_ft", O_CREAT | O_WRONLY, 0644);
  TEST_ASSERT_TRUE(fd >= 0);
  if (fd >= 0) {
    TEST_ASSERT_EQUAL_INT(0, ftruncate(fd, 50));
    struct stat st;
    TEST_ASSERT_EQUAL_INT(0, fstat(fd, &st));
    TEST_ASSERT_EQUAL_INT(50, (int)st.st_size);
    close(fd);
    unlink("/vfs_test_ft");
  }
}

/* 6. path_walk 穿非目录组件 → ENOTDIR(Linux 语义:自包含,不依赖 /dev 人口)。 */
void test_vfs_dispatch_path_walk_enotdir(void) {
  /* 在 FAT32 根建普通文件,再尝试穿它开子路径 → ENOTDIR */
  int fd = open("/vfs_test_enotdir", O_CREAT | O_WRONLY, 0644);
  TEST_ASSERT_TRUE(fd >= 0);
  if (fd >= 0)
    close(fd);
  int fd2 = open("/vfs_test_enotdir/sub", O_RDONLY);
  TEST_ASSERT_TRUE(fd2 < 0); /* 非目录做中间段 → ENOTDIR */
  if (fd2 >= 0)
    close(fd2);
  unlink("/vfs_test_enotdir");
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();
  RUN_TEST(test_vfs_dispatch_sysfs_create_denied);
  RUN_TEST(test_vfs_dispatch_sysfs_truncate_denied);
  RUN_TEST(test_vfs_dispatch_fat32_create_trunc);
  RUN_TEST(test_vfs_dispatch_fat32_truncate_grow);
  RUN_TEST(test_vfs_dispatch_fat32_ftruncate);
  RUN_TEST(test_vfs_dispatch_path_walk_enotdir);
  return UNITY_END();
}

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unity.h>
#include <xos/errno.h>

void setUp(void) {}
void tearDown(void) {}

/* ===== mount syscall error paths ===== */

void test_mount_dup(void) {
  int r = mount(NULL, "/dev", "devtmpfs", 0, NULL);
  TEST_ASSERT_EQUAL_INT(-1, r);
  TEST_ASSERT_EQUAL_INT(EBUSY, errno);
}

void test_mount_unknown_fs(void) {
  int r = mount(NULL, "/x", "nope", 0, NULL);
  TEST_ASSERT_EQUAL_INT(-1, r);
  TEST_ASSERT_EQUAL_INT(ENODEV, errno);
}

void test_mount_null_target(void) {
  int r = mount(NULL, NULL, "devtmpfs", 0, NULL);
  TEST_ASSERT_EQUAL_INT(-1, r);
}

/* ===== devtmpfs traversal (regression) ===== */

void test_open_dev_serial(void) {
  int fd = open("/dev/serial", O_RDWR);
  TEST_ASSERT_TRUE(fd >= 0);
  if (fd >= 0)
    close(fd);
}

void test_stat_dev(void) {
  struct stat st;
  int r = stat("/dev/serial", &st);
  TEST_ASSERT_EQUAL_INT(0, r);
  TEST_ASSERT_TRUE(S_ISCHR(st.st_mode));
  TEST_ASSERT_TRUE(st.st_ino > 0);
}

void test_stat_dev_dir(void) {
  struct stat st;
  int r = stat("/dev", &st);
  TEST_ASSERT_EQUAL_INT(0, r);
  TEST_ASSERT_TRUE(S_ISDIR(st.st_mode));
}

/* ===== devtmpfs getdents ===== */

void test_getdents_dev(void) {
  DIR *d = opendir("/dev");
  TEST_ASSERT_TRUE(d != NULL);
  if (!d)
    return;
  int found = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, "serial") == 0)
      found = 1;
  }
  closedir(d);
  TEST_ASSERT_TRUE(found);
}

/* ===== FAT32 root regression ===== */

void test_fat32_open(void) {
  int fd = open("/test/mount.elf", O_RDONLY);
  TEST_ASSERT_TRUE(fd >= 0);
  if (fd >= 0)
    close(fd);
}

void test_fat32_stat(void) {
  struct stat st;
  int r = stat("/test/mount.elf", &st);
  TEST_ASSERT_EQUAL_INT(0, r);
  TEST_ASSERT_TRUE(S_ISREG(st.st_mode));
}

void test_fat32_getdents_root(void) {
  DIR *d = opendir("/");
  TEST_ASSERT_TRUE(d != NULL);
  if (!d)
    return;
  int found_dev = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, "dev") == 0)
      found_dev = 1;
  }
  closedir(d);
  TEST_ASSERT_TRUE(found_dev);
}

/* ===== ino uniqueness ===== */

void test_ino_unique(void) {
  struct stat st1, st2;
  TEST_ASSERT_EQUAL_INT(0, stat("/dev/serial", &st1));
  /* sda block device should have a different ino */
  int r2 = stat("/dev/sda", &st2);
  if (r2 == 0) {
    TEST_ASSERT_TRUE(st1.st_ino != st2.st_ino);
  } else {
    TEST_ASSERT_TRUE(1); /* sda may not exist */
  }
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();

  RUN_TEST(test_mount_dup);
  RUN_TEST(test_mount_unknown_fs);
  RUN_TEST(test_mount_null_target);
  RUN_TEST(test_open_dev_serial);
  RUN_TEST(test_stat_dev);
  RUN_TEST(test_stat_dev_dir);
  RUN_TEST(test_getdents_dev);
  RUN_TEST(test_fat32_open);
  RUN_TEST(test_fat32_stat);
  RUN_TEST(test_fat32_getdents_root);
  RUN_TEST(test_ino_unique);

  return UNITY_END();
}

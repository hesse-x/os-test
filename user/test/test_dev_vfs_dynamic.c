/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_dev_vfs_dynamic.c — devtmpfs 设备表动态化回归(work2_design §5.5.1)。
 * 验证 >32 设备 + >16 子目录无 ENOMEM,getdents 枚举完整。桩 #ifdef TEST 门控:
 * sys_dev_create 造的 dyn_testN 是 driver_pid=current 的用户态占位设备(minor=0,
 * callbacks NULL),与真实设备同走 devtmpfs_create→kmalloc 路径,不污染非 TEST
 * 启动 设备空间。user 态 C 用 int 代 bool(无 stdbool)。 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <syscall.h>
#include <unistd.h>
#include <unity.h>

#define DYN_DEV_COUNT 40
#define DYN_SUB_COUNT 20

void setUp(void) {}
void tearDown(void) {}

/* >32 设备无 ENOMEM:循环造 40 个,全部返 0 */
static void test_dyn_dev_more_than_32(void) {
  for (int i = 0; i < DYN_DEV_COUNT; i++) {
    char name[32];
    snprintf(name, sizeof(name), "dyn_test%d", i);
    int r = sys_dev_create(name, -1, 0);
    TEST_ASSERT_EQUAL_INT(0, r);
  }
}

/* 造出的设备可 open(fd >= 0) */
static void test_dyn_dev_open(void) {
  char name[32];
  snprintf(name, sizeof(name), "dyn_test%d", DYN_DEV_COUNT / 2);
  char path[64];
  snprintf(path, sizeof(path), "/dev/%s", name);
  int fd = open(path, O_RDWR);
  TEST_ASSERT_TRUE(fd >= 0);
  if (fd >= 0)
    close(fd);
}

/* getdents 枚举完整:opendir("/dev") + readdir 确认 40 个 dyn_testN 全出现 */
static void test_dyn_dev_getdents(void) {
  DIR *d = opendir("/dev");
  TEST_ASSERT_TRUE(d != NULL);
  int seen[DYN_DEV_COUNT];
  for (int i = 0; i < DYN_DEV_COUNT; i++)
    seen[i] = 0;
  if (d) {
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
      for (int i = 0; i < DYN_DEV_COUNT; i++) {
        char want[32];
        snprintf(want, sizeof(want), "dyn_test%d", i);
        if (strcmp(e->d_name, want) == 0) {
          seen[i] = 1;
          break;
        }
      }
    }
    closedir(d);
  }
  for (int i = 0; i < DYN_DEV_COUNT; i++)
    TEST_ASSERT_TRUE(seen[i]);
}

/* >16 子目录无上限:循环造 subN/devX 触发 >16 个子目录,全部成功 */
static void test_dyn_subdir_more_than_16(void) {
  for (int i = 0; i < DYN_SUB_COUNT; i++) {
    char name[32];
    snprintf(name, sizeof(name), "sub%d/devX", i);
    int r = sys_dev_create(name, -1, 0);
    TEST_ASSERT_EQUAL_INT(0, r);
  }
}

/* 子目录 getdents:opendir("/dev/subN") 枚举其下设备 */
static void test_dyn_subdir_getdents(void) {
  char path[64];
  snprintf(path, sizeof(path), "/dev/sub%d", DYN_SUB_COUNT / 2);
  DIR *d = opendir(path);
  TEST_ASSERT_TRUE(d != NULL);
  int found = 0;
  if (d) {
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
      if (strcmp(e->d_name, "devX") == 0)
        found = 1;
    }
    closedir(d);
  }
  TEST_ASSERT_TRUE(found);
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();
  RUN_TEST(test_dyn_dev_more_than_32);
  RUN_TEST(test_dyn_dev_open);
  RUN_TEST(test_dyn_dev_getdents);
  RUN_TEST(test_dyn_subdir_more_than_16);
  RUN_TEST(test_dyn_subdir_getdents);
  return UNITY_END();
}

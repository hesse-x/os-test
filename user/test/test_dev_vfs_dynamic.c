/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// test_dev_vfs_dynamic.c — devtmpfs dynamic device-table regression
// (work2_design §5.5.1). Verifies >32 devices + >16 subdirs without ENOMEM,
// and complete getdents enumeration. Stubs gated by #ifdef TEST: the dyn_testN
// created via sys_dev_create are user-space placeholder devices (minor=0,
// callbacks NULL) with driver_pid=current, taking the same devtmpfs_create→
// kmalloc path as real devices, without polluting non-TEST boots. User C uses
// int for bool (no stdbool).
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <unity.h>
#include <xos/syscall_ext.h>

#define DYN_DEV_COUNT 40
#define DYN_SUB_COUNT 20

void setUp(void) {}
void tearDown(void) {}

// >32 devices without ENOMEM: create 40 in a loop, all return 0.
static void test_dyn_dev_more_than_32(void) {
  for (int i = 0; i < DYN_DEV_COUNT; i++) {
    char name[32];
    snprintf(name, sizeof(name), "dyn_test%d", i);
    int r = sys_dev_create(name, -1, 0);
    TEST_ASSERT_EQUAL_INT(0, r);
  }
}

// Created devices can be opened (fd >= 0).
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

// getdents enumeration complete: opendir("/dev") + readdir confirms all 40
// dyn_testN appear.
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

// >16 subdirs without cap: create subN/devX in a loop to trigger >16 subdirs,
// all succeed.
static void test_dyn_subdir_more_than_16(void) {
  for (int i = 0; i < DYN_SUB_COUNT; i++) {
    char name[32];
    snprintf(name, sizeof(name), "sub%d/devX", i);
    int r = sys_dev_create(name, -1, 0);
    TEST_ASSERT_EQUAL_INT(0, r);
  }
}

// Subdir getdents: opendir("/dev/subN") enumerates its devices.
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

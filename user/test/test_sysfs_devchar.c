/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <dirent.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void assert_file_contents(const char *path, const char *expected) {
  char buf[64] = {0};
  int fd = open(path, O_RDONLY);
  TEST_ASSERT_TRUE_MESSAGE(fd >= 0, path);
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  TEST_ASSERT_TRUE_MESSAGE(n > 0, path);
  buf[n] = '\0';
  TEST_ASSERT_EQUAL_STRING_MESSAGE(expected, buf, path);
  TEST_ASSERT_EQUAL_INT64_MESSAGE(0, read(fd, buf, sizeof(buf)), path);
  close(fd);
}

static void assert_devchar_node(const char *node, const char *devname) {
  char path[128];
  char buf[256] = {0};

  snprintf(path, sizeof(path), "/sys/dev/char/%s/uevent", node);
  int fd = open(path, O_RDONLY);
  TEST_ASSERT_TRUE_MESSAGE(fd >= 0, path);
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  TEST_ASSERT_TRUE(n > 0);
  buf[n] = '\0';

  char expected[64];
  snprintf(expected, sizeof(expected), "DEVNAME=%s\n", devname);
  TEST_ASSERT_NOT_NULL(strstr(buf, expected));
  snprintf(expected, sizeof(expected), "MAJOR=226\n");
  TEST_ASSERT_NOT_NULL(strstr(buf, expected));
  snprintf(expected, sizeof(expected), "MINOR=%s\n",
           strcmp(node, "226:0") == 0 ? "0" : "128");
  TEST_ASSERT_NOT_NULL(strstr(buf, expected));

  struct stat st;
  snprintf(path, sizeof(path), "/sys/dev/char/%s/device/drm", node);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, stat(path, &st), path);
  TEST_ASSERT_TRUE(S_ISDIR(st.st_mode));

  DIR *dir = opendir(path);
  TEST_ASSERT_NOT_NULL_MESSAGE(dir, path);
  bool found_card = false, found_render = false;
  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL) {
    found_card |= strcmp(ent->d_name, "card0") == 0;
    found_render |= strcmp(ent->d_name, "renderD128") == 0;
  }
  closedir(dir);
  TEST_ASSERT_TRUE(found_card);
  TEST_ASSERT_TRUE(found_render);

  snprintf(path, sizeof(path), "/sys/dev/char/%s/device/subsystem", node);
  n = readlink(path, buf, sizeof(buf) - 1);
  TEST_ASSERT_TRUE_MESSAGE(n > 0, path);
  buf[n] = '\0';
  TEST_ASSERT_EQUAL_STRING("/sys/bus/virtio", buf);

  snprintf(path, sizeof(path), "/sys/dev/char/%s/device/vendor", node);
  assert_file_contents(path, "0x1AF4\n");
  snprintf(path, sizeof(path), "/sys/dev/char/%s/device/device", node);
  assert_file_contents(path, "0x1050\n");
  snprintf(path, sizeof(path), "/sys/dev/char/%s/device/class", node);
  assert_file_contents(path, "0x038000\n");
  snprintf(path, sizeof(path), "/sys/dev/char/%s/device/subsystem_vendor",
           node);
  assert_file_contents(path, "0x1AF4\n");
  snprintf(path, sizeof(path), "/sys/dev/char/%s/device/subsystem_device",
           node);
  assert_file_contents(path, "0x1100\n");

  snprintf(path, sizeof(path), "/sys/dev/char/%s/device/uevent", node);
  assert_file_contents(path, "PCI_SLOT_NAME=0000:00:03.0\n");
}

static void test_card0_reverse_topology(void) {
  assert_devchar_node("226:0", "dri/card0");
}

static void test_render_node_reverse_topology(void) {
  assert_devchar_node("226:128", "dri/renderD128");
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_card0_reverse_topology);
  RUN_TEST(test_render_node_reverse_topology);
  return UNITY_END();
}

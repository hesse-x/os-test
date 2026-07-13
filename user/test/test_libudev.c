/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// libudev shim 阶段 A 回归测试（TEST 构建，依赖 evdev 真实注册态）。
// 覆盖 udev_design1.md §6.2：get_sysattr_value / syspath / enumerate /
// devnum 三边一致 / ID_INPUT_* 属性。

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unity.h>

#include "libudev.h"

void setUp(void) {}
void tearDown(void) {}

/* 选取实际存在的 evdev 设备作为测试目标。返回 /dev/input/eventN 路径，
 * 失败则 TEST_FAIL。 */
static const char *pick_evdev_devnode(void) {
  DIR *dir = opendir("/dev/input");
  if (!dir)
    return NULL;
  static char devnode[64];
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strncmp(entry->d_name, "event", 5) != 0)
      continue;
    snprintf(devnode, sizeof(devnode), "/dev/input/%s", entry->d_name);
    closedir(dir);
    return devnode;
  }
  closedir(dir);
  return NULL;
}

/* ---- P0: get_sysattr_value 读 sysfs 真文件 ---- */

void test_get_sysattr_name(void) {
  const char *dn = pick_evdev_devnode();
  TEST_ASSERT_NOT_NULL(dn);
  struct udev *u = udev_new();
  TEST_ASSERT_NOT_NULL(u);
  struct udev_device *d = udev_device_new_from_devnum(u, 'c', 0);
  /* 先靠扫描建立设备表；再用 devnum 匹配。下面统一用 from_subsystem_sysname
   * 兜底取设备，避免依赖 devnum 数值。 */
  (void)d;
  struct udev_device *dev =
      udev_device_new_from_subsystem_sysname(u, "input", strrchr(dn, '/') + 1);
  TEST_ASSERT_NOT_NULL(dev);
  const char *name = udev_device_get_sysattr_value(dev, "name");
  TEST_ASSERT_NOT_NULL(name);
  TEST_ASSERT_TRUE(strlen(name) > 0);
  udev_device_unref(dev);
  udev_unref(u);
}

void test_get_sysattr_vendor(void) {
  const char *dn = pick_evdev_devnode();
  TEST_ASSERT_NOT_NULL(dn);
  struct udev *u = udev_new();
  struct udev_device *dev =
      udev_device_new_from_subsystem_sysname(u, "input", strrchr(dn, '/') + 1);
  TEST_ASSERT_NOT_NULL(dev);
  const char *vendor = udev_device_get_sysattr_value(dev, "vendor");
  TEST_ASSERT_NOT_NULL(vendor);
  /* vendor 为 hex4，长度 > 0 */
  TEST_ASSERT_TRUE(strlen(vendor) > 0);
  udev_device_unref(dev);
  udev_unref(u);
}

void test_get_sysattr_product(void) {
  const char *dn = pick_evdev_devnode();
  TEST_ASSERT_NOT_NULL(dn);
  struct udev *u = udev_new();
  struct udev_device *dev =
      udev_device_new_from_subsystem_sysname(u, "input", strrchr(dn, '/') + 1);
  TEST_ASSERT_NOT_NULL(dev);
  const char *product = udev_device_get_sysattr_value(dev, "product");
  TEST_ASSERT_NOT_NULL(product);
  TEST_ASSERT_TRUE(strlen(product) > 0);
  udev_device_unref(dev);
  udev_unref(u);
}

void test_get_sysattr_enoent(void) {
  const char *dn = pick_evdev_devnode();
  TEST_ASSERT_NOT_NULL(dn);
  struct udev *u = udev_new();
  struct udev_device *dev =
      udev_device_new_from_subsystem_sysname(u, "input", strrchr(dn, '/') + 1);
  TEST_ASSERT_NOT_NULL(dev);
  /* 不存在的属性 → 内核 sysfs_lookup 返回 NULL → open 失败 → 返回 NULL */
  const char *v = udev_device_get_sysattr_value(dev, "no_such_attr_xyz");
  TEST_ASSERT_NULL(v);
  udev_device_unref(dev);
  udev_unref(u);
}

/* ---- P1: syspath 真实路径 ---- */

void test_syspath_real_path(void) {
  const char *dn = pick_evdev_devnode();
  TEST_ASSERT_NOT_NULL(dn);
  struct udev *u = udev_new();
  struct udev_device *dev =
      udev_device_new_from_subsystem_sysname(u, "input", strrchr(dn, '/') + 1);
  TEST_ASSERT_NOT_NULL(dev);
  const char *sp = udev_device_get_syspath(dev);
  TEST_ASSERT_NOT_NULL(sp);
  TEST_ASSERT_EQUAL_STRING("/sys/class/input/event0", sp);
  udev_device_unref(dev);
  udev_unref(u);
}

/* ---- P0(devnum): devnum 三边一致性 ---- */

void test_devnum_ino_consistency(void) {
  const char *dn = pick_evdev_devnode();
  TEST_ASSERT_NOT_NULL(dn);
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(dn, &st));

  struct udev *u = udev_new();
  /* libinput 路径：stat → st.st_rdev → new_from_devnum 匹配同一 ino 值 */
  struct udev_device *dev = udev_device_new_from_devnum(u, 'c', st.st_rdev);
  TEST_ASSERT_NOT_NULL(dev);
  TEST_ASSERT_EQUAL_INT((int)st.st_rdev, (int)udev_device_get_devnum(dev));
  udev_device_unref(dev);
  udev_unref(u);
}

/* ---- P2: enumerate ---- */

void test_enumerate_input(void) {
  struct udev *u = udev_new();
  struct udev_enumerate *e = udev_enumerate_new(u);
  TEST_ASSERT_NOT_NULL(e);
  TEST_ASSERT_EQUAL_INT(0, udev_enumerate_add_match_subsystem(e, "input"));
  TEST_ASSERT_EQUAL_INT(0, udev_enumerate_scan_devices(e));

  struct udev_list_entry *le = udev_enumerate_get_list_entry(e);
  TEST_ASSERT_NOT_NULL(le);
  /* /sys/class/input 下至少存在 event0 */
  int found_event0 = 0;
  for (; le; le = udev_list_entry_get_next(le)) {
    const char *name = udev_list_entry_get_name(le);
    if (strstr(name, "/sys/class/input/event0"))
      found_event0 = 1;
  }
  TEST_ASSERT_TRUE(found_event0);
  udev_enumerate_unref(e);
  udev_unref(u);
}

void test_enumerate_sysname_filter(void) {
  struct udev *u = udev_new();
  struct udev_enumerate *e = udev_enumerate_new(u);
  TEST_ASSERT_EQUAL_INT(0, udev_enumerate_add_match_subsystem(e, "input"));
  TEST_ASSERT_EQUAL_INT(0, udev_enumerate_add_match_sysname(e, "event*"));
  TEST_ASSERT_EQUAL_INT(0, udev_enumerate_scan_devices(e));

  struct udev_list_entry *le = udev_enumerate_get_list_entry(e);
  for (; le; le = udev_list_entry_get_next(le)) {
    const char *name = udev_list_entry_get_name(le);
    /* 过滤后每条 syspath 的末段都应以 "event" 开头 */
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    TEST_ASSERT_EQUAL_INT(0, strncmp(base, "event", 5));
  }
  udev_enumerate_unref(e);
  udev_unref(u);
}

void test_enumerate_empty_subsys(void) {
  struct udev *u = udev_new();
  struct udev_enumerate *e = udev_enumerate_new(u);
  TEST_ASSERT_EQUAL_INT(0, udev_enumerate_add_match_subsystem(e, "no_such"));
  TEST_ASSERT_EQUAL_INT(0, udev_enumerate_scan_devices(e));
  TEST_ASSERT_NULL(udev_enumerate_get_list_entry(e));
  udev_enumerate_unref(e);
  udev_unref(u);
}

/* ---- P0: ID_INPUT_* 属性 ---- */

void test_property_id_input(void) {
  const char *dn = pick_evdev_devnode();
  TEST_ASSERT_NOT_NULL(dn);
  struct udev *u = udev_new();
  struct udev_device *dev =
      udev_device_new_from_subsystem_sysname(u, "input", strrchr(dn, '/') + 1);
  TEST_ASSERT_NOT_NULL(dev);
  const char *v = udev_device_get_property_value(dev, "ID_INPUT");
  TEST_ASSERT_EQUAL_STRING("1", v);
  udev_device_unref(dev);
  udev_unref(u);
}

void test_property_id_keyboard(void) {
  const char *dn = pick_evdev_devnode();
  TEST_ASSERT_NOT_NULL(dn);
  struct udev *u = udev_new();
  struct udev_device *dev =
      udev_device_new_from_subsystem_sysname(u, "input", strrchr(dn, '/') + 1);
  TEST_ASSERT_NOT_NULL(dev);
  const char *v = udev_device_get_property_value(dev, "ID_INPUT_KEYBOARD");
  TEST_ASSERT_EQUAL_STRING("1", v);
  udev_device_unref(dev);
  udev_unref(u);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_get_sysattr_name);
  RUN_TEST(test_get_sysattr_vendor);
  RUN_TEST(test_get_sysattr_product);
  RUN_TEST(test_get_sysattr_enoent);
  RUN_TEST(test_syspath_real_path);
  RUN_TEST(test_devnum_ino_consistency);
  RUN_TEST(test_enumerate_input);
  RUN_TEST(test_enumerate_sysname_filter);
  RUN_TEST(test_enumerate_empty_subsys);
  RUN_TEST(test_property_id_input);
  RUN_TEST(test_property_id_keyboard);
  return UNITY_END();
}

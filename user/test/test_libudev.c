/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// libudev shim phase-A regression tests (TEST build; depends on real evdev
// registration state). Covers udev_design1.md §6.2: get_sysattr_value / syspath
// / enumerate / devnum three-way consistency / ID_INPUT_* properties.

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

// Pick an actually-present evdev device as the test target. Returns the
// /dev/input/eventN path, or NULL on failure.
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

// ---- P0: get_sysattr_value reads real sysfs files ----

void test_get_sysattr_name(void) {
  const char *dn = pick_evdev_devnode();
  TEST_ASSERT_NOT_NULL(dn);
  struct udev *u = udev_new();
  TEST_ASSERT_NOT_NULL(u);
  struct udev_device *d = udev_device_new_from_devnum(u, 'c', 0);
  // Rely on a scan to build the device table first, then match by devnum.
  // Below we uniformly use from_subsystem_sysname as a fallback to obtain the
  // device, avoiding dependence on the devnum value.
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
  // vendor is hex4, length > 0
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
  // Non-existent attr → kernel sysfs_lookup returns NULL → open fails → NULL.
  const char *v = udev_device_get_sysattr_value(dev, "no_such_attr_xyz");
  TEST_ASSERT_NULL(v);
  udev_device_unref(dev);
  udev_unref(u);
}

// ---- P1: syspath real path ----

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

// ---- P0(devnum): devnum three-way consistency ----

void test_devnum_ino_consistency(void) {
  const char *dn = pick_evdev_devnode();
  TEST_ASSERT_NOT_NULL(dn);
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(dn, &st));

  struct udev *u = udev_new();
  // libinput path: stat → st.st_rdev → new_from_devnum matches the same ino.
  struct udev_device *dev = udev_device_new_from_devnum(u, 'c', st.st_rdev);
  TEST_ASSERT_NOT_NULL(dev);
  TEST_ASSERT_EQUAL_INT((int)st.st_rdev, (int)udev_device_get_devnum(dev));
  udev_device_unref(dev);
  udev_unref(u);
}

// ---- P2: enumerate ----

void test_enumerate_input(void) {
  struct udev *u = udev_new();
  struct udev_enumerate *e = udev_enumerate_new(u);
  TEST_ASSERT_NOT_NULL(e);
  TEST_ASSERT_EQUAL_INT(0, udev_enumerate_add_match_subsystem(e, "input"));
  TEST_ASSERT_EQUAL_INT(0, udev_enumerate_scan_devices(e));

  struct udev_list_entry *le = udev_enumerate_get_list_entry(e);
  TEST_ASSERT_NOT_NULL(le);
  // /sys/class/input should contain at least event0.
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
    // After filtering, every syspath's last segment must start with "event".
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

// ---- P0: ID_INPUT_* properties ----

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

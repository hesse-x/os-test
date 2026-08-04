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
  char selected[32] = "";
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strncmp(entry->d_name, "event", 5) != 0)
      continue;
    // Directory iteration order is unspecified. The platform registers the
    // keyboard first (event0), so consistently choose the lowest event node.
    if (!selected[0] || strcmp(entry->d_name, selected) < 0)
      snprintf(selected, sizeof(selected), "%s", entry->d_name);
  }
  closedir(dir);
  if (!selected[0])
    return NULL;
  snprintf(devnode, sizeof(devnode), "/dev/input/%s", selected);
  return devnode;
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

void test_enumerate_repeated_scan(void) {
  struct udev *u = udev_new();
  struct udev_enumerate *e = udev_enumerate_new(u);
  TEST_ASSERT_NOT_NULL(e);
  TEST_ASSERT_EQUAL_INT(0, udev_enumerate_add_match_subsystem(e, "drm"));
  TEST_ASSERT_EQUAL_INT(0, udev_enumerate_add_match_sysname(e, "card[0-9]*"));
  TEST_ASSERT_EQUAL_INT(0, udev_enumerate_scan_devices(e));
  struct udev_list_entry *first = udev_enumerate_get_list_entry(e);
  TEST_ASSERT_NOT_NULL(first);
  char first_path[128];
  snprintf(first_path, sizeof(first_path), "%s",
           udev_list_entry_get_name(first));
  TEST_ASSERT_EQUAL_INT(0, udev_enumerate_scan_devices(e));
  first = udev_enumerate_get_list_entry(e);
  TEST_ASSERT_NOT_NULL(first);
  TEST_ASSERT_EQUAL_STRING(first_path, udev_list_entry_get_name(first));
  udev_enumerate_unref(e);
  udev_unref(u);
}

void test_enumerate_drm_primary(void) {
  struct udev *u = udev_new();
  struct udev_enumerate *e = udev_enumerate_new(u);
  TEST_ASSERT_NOT_NULL(e);
  TEST_ASSERT_EQUAL_INT(0, udev_enumerate_add_match_subsystem(e, "drm"));
  TEST_ASSERT_EQUAL_INT(0, udev_enumerate_add_match_sysname(e, "card[0-9]*"));
  TEST_ASSERT_EQUAL_INT(0, udev_enumerate_scan_devices(e));
  int primary_count = 0;
  struct udev_list_entry *entry;
  udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(e)) {
    struct udev_device *device =
        udev_device_new_from_syspath(u, udev_list_entry_get_name(entry));
    if (!device)
      continue;
    TEST_ASSERT_EQUAL_STRING("drm", udev_device_get_subsystem(device));
    TEST_ASSERT_EQUAL_STRING("/dev/dri/card0", udev_device_get_devnode(device));
    TEST_ASSERT_EQUAL_PTR(u, udev_device_get_udev(device));
    primary_count++;
    udev_device_unref(device);
  }
  TEST_ASSERT_EQUAL_INT(1, primary_count);
  udev_enumerate_unref(e);
  udev_unref(u);
}

void test_drm_device_rejects_connector_syspath(void) {
  struct udev *u = udev_new();
  errno = 0;
  TEST_ASSERT_NULL(
      udev_device_new_from_syspath(u, "/sys/class/drm/card0-DP-1"));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
  TEST_ASSERT_NULL(udev_device_new_from_syspath(u, "/sys/class/drm/../card0"));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
  udev_unref(u);
}

static size_t build_frame(unsigned char *frame, size_t capacity,
                          const char *payload, size_t payload_len) {
  if (capacity < UDEV_FRAME_HEADER_SIZE + payload_len)
    return 0;
  memcpy(frame, "UDEV", 4);
  frame[4] = 1;
  frame[5] = frame[6] = frame[7] = 0;
  frame[8] = (unsigned char)(payload_len & 0xff);
  frame[9] = (unsigned char)((payload_len >> 8) & 0xff);
  frame[10] = (unsigned char)((payload_len >> 16) & 0xff);
  frame[11] = (unsigned char)((payload_len >> 24) & 0xff);
  memcpy(frame + UDEV_FRAME_HEADER_SIZE, payload, payload_len);
  return UDEV_FRAME_HEADER_SIZE + payload_len;
}

void test_monitor_filter_contract(void) {
  struct udev *u = udev_new();
  struct udev_monitor *monitor = udev_monitor_new_from_netlink(u, "udev");
  TEST_ASSERT_NOT_NULL(monitor);
  TEST_ASSERT_EQUAL_INT(
      0, udev_monitor_filter_add_match_subsystem_devtype(monitor, "drm", NULL));
  TEST_ASSERT_EQUAL_INT(
      0, udev_monitor_filter_add_match_subsystem_devtype(monitor, "drm", NULL));
  TEST_ASSERT_EQUAL_INT(
      -EOPNOTSUPP,
      udev_monitor_filter_add_match_subsystem_devtype(monitor, "input", NULL));
  udev_monitor_unref(monitor);
  udev_unref(u);
}

void test_monitor_invalid_header_is_fatal(void) {
  int fds[2];
  TEST_ASSERT_EQUAL_INT(0, pipe2(fds, O_NONBLOCK | O_CLOEXEC));
  struct udev *u = udev_new();
  struct udev_monitor *monitor = udev_monitor_new_from_netlink(u, "udev");
  TEST_ASSERT_NOT_NULL(monitor);
  monitor->pipe_fd = fds[0];
  monitor->subscribed = 1;
  unsigned char header[UDEV_FRAME_HEADER_SIZE] = {'B', 'A', 'D', '!', 1, 0,
                                                  0,   0,   1,   0,   0, 0};
  TEST_ASSERT_EQUAL_INT((int)sizeof(header),
                        write(fds[1], header, sizeof(header)));
  errno = 0;
  TEST_ASSERT_NULL(udev_monitor_receive_device(monitor));
  TEST_ASSERT_EQUAL_INT(EPROTO, errno);
  TEST_ASSERT_EQUAL_INT(-1, monitor->pipe_fd);
  close(fds[1]);
  udev_monitor_unref(monitor);
  udev_unref(u);
}

void test_monitor_three_malformed_payloads_are_fatal(void) {
  int fds[2];
  TEST_ASSERT_EQUAL_INT(0, pipe2(fds, O_NONBLOCK | O_CLOEXEC));
  struct udev *u = udev_new();
  struct udev_monitor *monitor = udev_monitor_new_from_netlink(u, "udev");
  TEST_ASSERT_NOT_NULL(monitor);
  monitor->pipe_fd = fds[0];
  monitor->subscribed = 1;
  static const char payload[] = "UNKNOWN=value\0";
  unsigned char frame[64];
  size_t frame_len =
      build_frame(frame, sizeof(frame), payload, sizeof(payload) - 1);
  TEST_ASSERT_TRUE(frame_len > 0);
  for (int i = 0; i < 3; i++)
    TEST_ASSERT_EQUAL_INT((int)frame_len, write(fds[1], frame, frame_len));
  errno = 0;
  TEST_ASSERT_NULL(udev_monitor_receive_device(monitor));
  TEST_ASSERT_EQUAL_INT(EPROTO, errno);
  TEST_ASSERT_EQUAL_INT(-1, monitor->pipe_fd);
  close(fds[1]);
  udev_monitor_unref(monitor);
  udev_unref(u);
}

void test_monitor_frame_split_and_filter(void) {
  static const char input_payload[] =
      "ACTION=add\0DEVNAME=/dev/input/event0\0SUBSYSTEM=input\0DEVNUM=10\0";
  static const char drm_payload[] =
      "ACTION=remove\0DEVNAME=/dev/dri/card0\0SUBSYSTEM=drm\0DEVNUM=20\0";
  unsigned char frames[512];
  size_t first = build_frame(frames, sizeof(frames), input_payload,
                             sizeof(input_payload) - 1);
  size_t second = build_frame(frames + first, sizeof(frames) - first,
                              drm_payload, sizeof(drm_payload) - 1);
  int fds[2];
  TEST_ASSERT_EQUAL_INT(0, pipe2(fds, O_NONBLOCK | O_CLOEXEC));
  struct udev *u = udev_new();
  struct udev_monitor *monitor = udev_monitor_new_from_netlink(u, "udev");
  TEST_ASSERT_NOT_NULL(monitor);
  TEST_ASSERT_EQUAL_INT(
      0, udev_monitor_filter_add_match_subsystem_devtype(monitor, "drm", NULL));
  monitor->pipe_fd = fds[0];
  monitor->subscribed = 1;

  for (size_t i = 0; i + 1 < first + second; i++) {
    TEST_ASSERT_EQUAL_INT(1, write(fds[1], frames + i, 1));
    errno = 0;
    TEST_ASSERT_NULL(udev_monitor_receive_device(monitor));
    TEST_ASSERT_EQUAL_INT(EAGAIN, errno);
  }
  TEST_ASSERT_EQUAL_INT(1, write(fds[1], frames + first + second - 1, 1));
  struct udev_device *device = udev_monitor_receive_device(monitor);
  TEST_ASSERT_NOT_NULL(device);
  TEST_ASSERT_EQUAL_STRING("drm", udev_device_get_subsystem(device));
  TEST_ASSERT_EQUAL_STRING("card0", udev_device_get_sysname(device));
  TEST_ASSERT_EQUAL_STRING("remove", udev_device_get_action(device));
  udev_device_unref(device);
  close(fds[1]);
  udev_monitor_unref(monitor);
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
  RUN_TEST(test_enumerate_repeated_scan);
  RUN_TEST(test_enumerate_drm_primary);
  RUN_TEST(test_drm_device_rejects_connector_syspath);
  RUN_TEST(test_monitor_filter_contract);
  RUN_TEST(test_monitor_invalid_header_is_fatal);
  RUN_TEST(test_monitor_three_malformed_payloads_are_fatal);
  RUN_TEST(test_monitor_frame_split_and_filter);
  RUN_TEST(test_property_id_input);
  RUN_TEST(test_property_id_keyboard);
  return UNITY_END();
}

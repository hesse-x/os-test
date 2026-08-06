/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// udevd daemon end-to-end test (TEST build).
// Covers udev_design.md §8.5.2: AF_UNIX connect + SCM_RIGHTS pipe fd +
// monitor uevent receive + coldplug snapshot of existing devices.
// Requires udevd running (init spawned it) and evdev registered (event0).
// Coldplug fires from udevd startup over /sys/class/input/event0; a second
// round fires after the first client connects. No test device is registered
// for coldplug.

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <unity.h>

#include "libudev.h"
#include <linux/input.h> // BUS_USB(dev_props.bustype)
#include <sys/device.h>

void setUp(void) {}
void tearDown(void) {}

// monitor handshake: enable_receiving succeeds ⇒ connected /run/udev/socket
// and received the pipe rd fd via SCM_RIGHTS. Returns -ENOENT if udevd is not
// running or the socket is absent; this test requires udevd, so assert == 0.
void test_monitor_enable_receiving(void) {
  struct udev *u = udev_new();
  TEST_ASSERT_NOT_NULL(u);
  struct udev_monitor *m = udev_monitor_new_from_netlink(u, "udev");
  TEST_ASSERT_NOT_NULL(m);
  TEST_ASSERT_EQUAL_INT(0, udev_monitor_enable_receiving(m));
  int fd = udev_monitor_get_fd(m);
  TEST_ASSERT_TRUE(fd >= 0); // pipe rd fd, epoll-able
  TEST_ASSERT_TRUE(fcntl(fd, F_GETFL) & O_NONBLOCK);
  TEST_ASSERT_TRUE(fcntl(fd, F_GETFD) & FD_CLOEXEC);
  udev_monitor_unref(m);
  udev_unref(u);
}

// get_fd returns the pipe rd fd (not -1), consistent with
// post-enable_receiving.
void test_monitor_get_fd(void) {
  struct udev *u = udev_new();
  struct udev_monitor *m = udev_monitor_new_from_netlink(u, "udev");
  TEST_ASSERT_EQUAL_INT(0, udev_monitor_enable_receiving(m));
  int fd = udev_monitor_get_fd(m);
  TEST_ASSERT_TRUE(fd >= 0);
  udev_monitor_unref(m);
  udev_unref(u);
}

// coldplug snapshot: after the first client connects, udevd fires a coldplug
// round; the existing event0 add arrives via the pipe. receive_device parses a
// udev_device with action == "add" and sysname starting with "event".
// A short poll wait covers udevd's rebroadcast loop; fail if no event in 5s
// (non-blocking for the framework).
static struct udev_device *recv_with_timeout(struct udev_monitor *m, int ms) {
  int fd = udev_monitor_get_fd(m);
  if (fd < 0)
    return NULL;
  struct pollfd pfd = {fd, POLLIN, 0};
  int r = poll(&pfd, 1, ms);
  if (r <= 0)
    return NULL;
  return udev_monitor_receive_device(m);
}

void test_monitor_receive_coldplug_add(void) {
  struct udev *u = udev_new();
  struct udev_monitor *m = udev_monitor_new_from_netlink(u, "udev");
  TEST_ASSERT_EQUAL_INT(0, udev_monitor_enable_receiving(m));
  struct udev_device *d = recv_with_timeout(m, 5000);
  TEST_ASSERT_NOT_NULL(d);
  const char *action = udev_device_get_action(d);
  TEST_ASSERT_NOT_NULL(action);
  TEST_ASSERT_EQUAL_STRING("add", action);
  const char *sysname = udev_device_get_sysname(d);
  TEST_ASSERT_NOT_NULL(sysname);
  TEST_ASSERT_EQUAL_INT(0, strncmp(sysname, "event", 5));
  udev_device_unref(d);
  udev_monitor_unref(m);
  udev_unref(u);
}

// monitor device properties come from pipe KV (§5.3/§6 grill decision: match
// Linux libudev — properties arrive with the uevent KV, stored in
// device->props, get_property_value reads the in-memory table, not the db).
// After coldplug add of event0, get_property_value("ID_INPUT") == "1".
void test_monitor_device_property_id_input(void) {
  struct udev *u = udev_new();
  struct udev_monitor *m = udev_monitor_new_from_netlink(u, "udev");
  TEST_ASSERT_EQUAL_INT(0, udev_monitor_enable_receiving(m));
  struct udev_device *d = recv_with_timeout(m, 5000);
  if (!d) {
    udev_monitor_unref(m);
    udev_unref(u);
    TEST_FAIL_MESSAGE("no monitor event within timeout");
  }
  const char *v = udev_device_get_property_value(d, "ID_INPUT");
  TEST_ASSERT_EQUAL_STRING("1", v);
  udev_device_unref(d);
  udev_monitor_unref(m);
  udev_unref(u);
}

// The project-owned event1 identity gets an explicit 1000 DPI baseline in
// both the monitor payload (coldplug path) and the persistent udev database.
void test_virtual_mouse_dpi_property(void) {
  struct udev *u = udev_new();
  struct udev_monitor *m = udev_monitor_new_from_netlink(u, "udev");
  TEST_ASSERT_EQUAL_INT(0, udev_monitor_enable_receiving(m));

  struct udev_device *mouse = NULL;
  for (int i = 0; i < 8 && !mouse; i++) {
    struct udev_device *d = recv_with_timeout(m, 1000);
    if (!d)
      break;
    const char *sysname = udev_device_get_sysname(d);
    if (sysname && strcmp(sysname, "event1") == 0)
      mouse = d;
    else
      udev_device_unref(d);
  }
  TEST_ASSERT_NOT_NULL(mouse);
  TEST_ASSERT_EQUAL_STRING("1000",
                           udev_device_get_property_value(mouse, "MOUSE_DPI"));
  udev_device_unref(mouse);
  udev_monitor_unref(m);

  mouse = udev_device_new_from_subsystem_sysname(u, "input", "event1");
  TEST_ASSERT_NOT_NULL(mouse);
  TEST_ASSERT_EQUAL_STRING("1000",
                           udev_device_get_property_value(mouse, "MOUSE_DPI"));
  udev_device_unref(mouse);
  udev_unref(u);
}

// A successful filter registration must be retained by the monitor.
void test_monitor_filter_registration(void) {
  struct udev *u = udev_new();
  struct udev_monitor *m = udev_monitor_new_from_netlink(u, "udev");
  TEST_ASSERT_EQUAL_INT(
      0, udev_monitor_filter_add_match_subsystem_devtype(m, "input", NULL));
  udev_monitor_unref(m);
  udev_unref(u);
}

// Two-step registration stub (mirrors test_sysfs.c register_event1):
// device_register_shm creates the devtmpfs node → device_set_meta builds the
// sysfs subtree and nl_uevent_broadcast("add") (devtmpfs.c:776-777) → udevd
// receives the uevent → enriches → broadcasts into the client pipe.
// Gated by #ifdef TEST, decoupled from real device sources.
// No device-removal API and uevent_store accepts only "add" (sysfs.c:316
// rejects remove), so test devices are not cleaned up (matches existing
// test_udevd_db.c / test_sysfs.c behavior; TEST image retains the traces).
static void trigger_test_device(const char *name, uint32_t minor) {
  TEST_ASSERT_EQUAL_INT(0, device_register_shm(name, -1, minor));
  struct dev_props props = {.bustype = BUS_USB,
                            .vendor = 0x0002,
                            .product = 0x0002,
                            .version = 0x0001};
  strncpy(props.name, "evdev test dev", sizeof(props.name) - 1);
  props.name[sizeof(props.name) - 1] = '\0';
  TEST_ASSERT_EQUAL_INT(0, device_set_meta(name, "input", "evdev", &props));
}

// hotplug add (§4.1): the stub device goes through two-step registration,
// triggering a real add uevent → udevd → enrich → pipe. Exercises the monitor
// end-to-end (not the coldplug snapshot path).
// enable_receiving triggers a coldplug via udevd accept_client; drain the
// event0 coldplug add first, then trigger event9 and read until sysname ==
// event9 (avoid mistaking the coldplug event0).
void test_monitor_hotplug_add(void) {
  struct udev *u = udev_new();
  TEST_ASSERT_NOT_NULL(u);
  struct udev_monitor *m = udev_monitor_new_from_netlink(u, "udev");
  TEST_ASSERT_NOT_NULL(m);
  TEST_ASSERT_EQUAL_INT(0, udev_monitor_enable_receiving(m));
  // Drain coldplug add triggered by enable_receiving (event0 etc.);
  // short poll timeout ⇒ no data means drained.
  for (int i = 0; i < 8; i++) {
    struct udev_device *stale = recv_with_timeout(m, 200);
    if (!stale)
      break;
    udev_device_unref(stale);
  }
  trigger_test_device("input/event9", 9);
  // Read add events until event9 arrives (coldplug residue vs event9 race).
  struct udev_device *d = NULL;
  for (int i = 0; i < 8 && !d; i++) {
    struct udev_device *e = recv_with_timeout(m, 5000);
    if (!e)
      break;
    if (strcmp(udev_device_get_action(e), "add") == 0 &&
        strcmp(udev_device_get_sysname(e), "event9") == 0) {
      d = e;
    } else {
      udev_device_unref(e);
    }
  }
  TEST_ASSERT_NOT_NULL(d);
  TEST_ASSERT_EQUAL_STRING("add", udev_device_get_action(d));
  TEST_ASSERT_EQUAL_STRING("event9", udev_device_get_sysname(d));
  udev_device_unref(d);
  udev_monitor_unref(m);
  udev_unref(u);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_monitor_enable_receiving);
  RUN_TEST(test_monitor_get_fd);
  RUN_TEST(test_monitor_receive_coldplug_add);
  RUN_TEST(test_monitor_device_property_id_input);
  RUN_TEST(test_virtual_mouse_dpi_property);
  RUN_TEST(test_monitor_filter_registration);
  RUN_TEST(test_monitor_hotplug_add);
  return UNITY_END();
}

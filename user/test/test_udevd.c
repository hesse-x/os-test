/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// udevd daemon 通道端到端测试(TEST 构建)。
// 覆盖 udev_design.md §8.5.2:AF_UNIX connect + SCM_RIGHTS 收 pipe fd +
// monitor 收 uevent + coldplug 现有设备 add 快照。
// 依赖:udevd 真实运行态(init 已 spawn udevd)+ evdev 注册态(event0)。
// 桩触发:本测试不注册测试设备——coldplug 路径由 udevd 启动时对
// /sys/class/input/event0 触发,首个 client connect 后再触发一轮。

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <unistd.h>
#include <unity.h>

#include "libudev.h"

void setUp(void) {}
void tearDown(void) {}

/* monitor 通道握手:enable_receiving 成功表示 connect /run/udev/socket +
 * SCM_RIGHTS 收到 pipe rd fd。udevd 未起/socket 不存在时返 -ENOENT——
 * 测试需 udevd 已运行,故断言 == 0。 */
void test_monitor_enable_receiving(void) {
  struct udev *u = udev_new();
  TEST_ASSERT_NOT_NULL(u);
  struct udev_monitor *m = udev_monitor_new_from_netlink(u, "udev");
  TEST_ASSERT_NOT_NULL(m);
  TEST_ASSERT_EQUAL_INT(0, udev_monitor_enable_receiving(m));
  int fd = udev_monitor_get_fd(m);
  TEST_ASSERT_TRUE(fd >= 0); /* pipe rd fd 可 epoll */
  udev_monitor_unref(m);
  udev_unref(u);
}

/* get_fd 返 pipe rd fd(非 -1),与 enable_receiving 后状态一致。 */
void test_monitor_get_fd(void) {
  struct udev *u = udev_new();
  struct udev_monitor *m = udev_monitor_new_from_netlink(u, "udev");
  TEST_ASSERT_EQUAL_INT(0, udev_monitor_enable_receiving(m));
  int fd = udev_monitor_get_fd(m);
  TEST_ASSERT_TRUE(fd >= 0);
  udev_monitor_unref(m);
  udev_unref(u);
}

/* coldplug 快照:首个 client connect 后 udevd 触发一轮 coldplug,
 * 现有设备 event0 的 add 经 pipe 到达 client。receive_device 解析出
 * udev_device,action=="add",sysname 以 "event" 开头。
 * 注:pipe 读可能需短暂等待(udevd 重广播回环),用 poll 超时兜底;
 * 若 5s 内无事件判失败(不阻塞测试框架)。 */
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

/* monitor device 的 property(ID_INPUT)来自 pipe KV(§5.3/§6 grill 决议:
 * 对齐 Linux libudev——monitor device 的 property 随 uevent KV 到达 client,
 * 存 device->props,get_property_value 查内存表,不查 db)。
 * coldplug add 收到 event0 后,get_property_value("ID_INPUT") 应为 "1"。 */
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

/* filter 本轮 no-op stub,返 0(不报错)。 */
void test_monitor_filter_noop(void) {
  struct udev *u = udev_new();
  struct udev_monitor *m = udev_monitor_new_from_netlink(u, "udev");
  TEST_ASSERT_EQUAL_INT(
      0, udev_monitor_filter_add_match_subsystem_devtype(m, "input", NULL));
  udev_monitor_unref(m);
  udev_unref(u);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_monitor_enable_receiving);
  RUN_TEST(test_monitor_get_fd);
  RUN_TEST(test_monitor_receive_coldplug_add);
  RUN_TEST(test_monitor_device_property_id_input);
  RUN_TEST(test_monitor_filter_noop);
  return UNITY_END();
}

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
#include <sys/device.h>
#include <xos/input.h> /* BUS_USB(dev_props.bustype) */

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

/* 两步注册桩(对齐 test_sysfs.c:register_event1 同源理念):device_register_shm
 * 建 devtmpfs 节点 → device_set_meta 建 sysfs 子树 + nl_uevent_broadcast("add")
 * (devtmpfs.c:776-777) → udevd 收 uevent → 补全 → 广播进 client pipe。
 * 桩作 #ifdef TEST 额外测试节点门控,不和真实设备来源耦合
 * ([[evdev-real-device-source-untouched-this-round]])。
 * 注:无设备移除 API + uevent_store 只接受 "add"(sysfs.c:316 拒 remove),
 * 故不清理测试设备(同 test_udevd_db.c/test_sysfs.c 既有做法,TEST 镜像留痕)。 */
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

/* 热插拔 add(§4.1):桩设备走两步注册触发真实 add uevent → udevd 收 → 补全
 * → 广播进 pipe。验证 monitor 端到端(非 coldplug 快照路径)。
 * 注:enable_receiving 时 udevd accept_client 会重触发 coldplug,先排空 event0
 * 的 coldplug add,再触发 event9,在有限轮内读到 sysname==event9 即通过
 * (避免误收 coldplug 的 event0)。 */
void test_monitor_hotplug_add(void) {
  struct udev *u = udev_new();
  TEST_ASSERT_NOT_NULL(u);
  struct udev_monitor *m = udev_monitor_new_from_netlink(u, "udev");
  TEST_ASSERT_NOT_NULL(m);
  TEST_ASSERT_EQUAL_INT(0, udev_monitor_enable_receiving(m));
  /* 排空 enable_receiving 触发的 coldplug add(event0 等),poll
   * 短超时:无数据即排空。 */
  for (int i = 0; i < 8; i++) {
    struct udev_device *stale = recv_with_timeout(m, 200);
    if (!stale)
      break;
    udev_device_unref(stale);
  }
  trigger_test_device("input/event9", 9);
  /* 在有限轮内读 add 事件直到 event9 到达(coldplug 残余与 event9 竞态兜底)。 */
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
  RUN_TEST(test_monitor_filter_noop);
  RUN_TEST(test_monitor_hotplug_add);
  return UNITY_END();
}

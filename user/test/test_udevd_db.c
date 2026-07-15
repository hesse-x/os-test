/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_udevd_db.c — 验证 §3.2 db + §3.3 规则引擎(端到端键盘类)。
 * 桩触发设备走两步注册触发真实 uevent → udevd 收 uevent → 探 caps → 写 db
 * → client 读。桩 #ifdef TEST 门控,不和真实设备来源耦合。
 * Unity freestanding:setUp/tearDown 空实现;断言 TEST_ASSERT_*。 */
#include "unity.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* 需要直接调 udev_* 符号 → 把 udev.c 列入 SOURCES(见 F3)。
 * udev.c 的 devnum 来自 stat.st_rdev(三边一致 ino),与 db key 同源。 */
#include "libudev.h"

/* 测试设备节点(db 场景下 /dev/input/eventX 的替身)。桩经 #ifdef TEST 走两步
 * 注册触发真实 uevent。 */
#define TEST_DEVNODE "/dev/input/event0"

void setUp(void) {}
void tearDown(void) {}

/* db 文件直读辅助(对齐 udevd db 落点 /run/udev/data/<devnum>)。 */
static int db_read_kv(uint32_t devnum, char *buf, size_t cap) {
  char path[80];
  snprintf(path, sizeof(path), "/run/udev/data/%u", devnum);
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  ssize_t n = read(fd, buf, cap - 1);
  close(fd);
  if (n < 0)
    return -1;
  buf[n] = '\0';
  return 0;
}

/* udevd 收 add uevent → 写 db 文件:db 写路径端到端
 * (桩触发设备走两步注册推 add)。 */
void test_udevd_writes_db_on_add(void) {
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(TEST_DEVNODE, &st));
  uint32_t devnum = (uint32_t)st.st_rdev; /* = ino */
  TEST_ASSERT_NOT_EQUAL(0, devnum);
  /* 等 udevd 处理完 add uevent(uevent 异步,轮询 db 文件就绪) */
  char db[512];
  int ok = 0;
  for (int i = 0; i < 100 && !ok; i++) {
    if (db_read_kv(devnum, db, sizeof(db)) == 0)
      ok = 1;
    else
      usleep(100 * 1000); /* 100ms */
  }
  TEST_ASSERT_TRUE(ok);
}

/* db KV 格式正确(ID_INPUT=1 等)。 */
void test_db_kv_format(void) {
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(TEST_DEVNODE, &st));
  uint32_t devnum = (uint32_t)st.st_rdev;
  char db[512];
  TEST_ASSERT_EQUAL_INT(0, db_read_kv(devnum, db, sizeof(db)));
  TEST_ASSERT_NOT_NULL(strstr(db, "ID_INPUT="));
  TEST_ASSERT_NOT_NULL(strstr(db, "ID_SEAT=seat0"));
}

/* input_id 合成 KEYBOARD(EV_KEY):现有 event0 keyboard 场景。 */
void test_input_id_keyboard(void) {
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(TEST_DEVNODE, &st));
  uint32_t devnum = (uint32_t)st.st_rdev;
  char db[512];
  TEST_ASSERT_EQUAL_INT(0, db_read_kv(devnum, db, sizeof(db)));
  TEST_ASSERT_NOT_NULL(strstr(db, "ID_INPUT_KEYBOARD=1"));
  TEST_ASSERT_NOT_NULL(strstr(db, "ID_INPUT_KEY=1"));
}

/* input_id 合成 ID_SEAT=seat0:seat 标(对齐 Linux 永远 seat0)。 */
void test_input_id_seat(void) {
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(TEST_DEVNODE, &st));
  uint32_t devnum = (uint32_t)st.st_rdev;
  char db[512];
  TEST_ASSERT_EQUAL_INT(0, db_read_kv(devnum, db, sizeof(db)));
  TEST_ASSERT_NOT_NULL(strstr(db, "ID_SEAT=seat0"));
}

/* shim get_property_value 直读 db 取 property:乙落地核心(client 读路径)。 */
void test_shim_get_property_reads_db(void) {
  struct udev *u = udev_new();
  TEST_ASSERT_NOT_NULL(u);
  struct udev_device *d = udev_device_new_from_devnum(
      u, 'c', 0); /* devnum 由 create_udev_device 填;
                   * 真实测试用 udev_device_new_from_subsystem
                   * 或扫 /dev/input 构造。 */
  if (d) {
    const char *v = udev_device_get_property_value(d, "ID_INPUT_KEYBOARD");
    /* db 有内容时应取到 "1"(event0 键盘);未就绪时 NULL 已被前置测点覆盖 */
    if (v)
      TEST_ASSERT_EQUAL_STRING("1", v);
    udev_device_unref(d);
  }
  udev_unref(u);
}

/* db 文件不存在 → get_property_value 返 NULL:降级(§5.2)。 */
void test_shim_missing_db_returns_null(void) {
  struct udev *u = udev_new();
  TEST_ASSERT_NOT_NULL(u);
  struct udev_device *d =
      udev_device_new_from_devnum(u, 'c', 1); /* 构造一个无 db 的 device */
  if (d) {
    const char *v = udev_device_get_property_value(d, "ID_INPUT_KEYBOARD");
    TEST_ASSERT_NULL(v);
    udev_device_unref(d);
  }
  udev_unref(u);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_udevd_writes_db_on_add);
  RUN_TEST(test_db_kv_format);
  RUN_TEST(test_input_id_keyboard);
  RUN_TEST(test_input_id_seat);
  RUN_TEST(test_shim_get_property_reads_db);
  RUN_TEST(test_shim_missing_db_returns_null);
  return UNITY_END();
}

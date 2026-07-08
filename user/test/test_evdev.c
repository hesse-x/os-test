/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <fcntl.h>
#include <freebsd/input.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/process.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syscall.h>
#include <unistd.h>
#include <xos/errno.h>

#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_evdev_version(void) {
  int fd = open("/dev/input/event0", O_RDWR);
  TEST_ASSERT_GREATER_OR_EQUAL(0, fd);
  int32_t v = 0;
  int rc = ioctl(fd, EVIOCGVERSION, &v);
  TEST_ASSERT_EQUAL(0, rc);
  TEST_ASSERT_EQUAL(EV_VERSION, v);
  close(fd);
}

static void test_evdev_id(void) {
  int fd = open("/dev/input/event0", O_RDWR);
  TEST_ASSERT_GREATER_OR_EQUAL(0, fd);
  struct input_id id = {};
  int rc = ioctl(fd, EVIOCGID, &id);
  TEST_ASSERT_EQUAL(0, rc);
  TEST_ASSERT_EQUAL(0x03, id.bustype);
  TEST_ASSERT_EQUAL(0x0001, id.vendor);
  TEST_ASSERT_EQUAL(0x0001, id.product);
  TEST_ASSERT_EQUAL(0x0001, id.version);
  close(fd);
}

static void test_evdev_name(void) {
  int fd = open("/dev/input/event0", O_RDWR);
  TEST_ASSERT_GREATER_OR_EQUAL(0, fd);
  char buf[48] = {};
  int rc = ioctl(fd, EVIOCGNAME(48), buf);
  TEST_ASSERT_EQUAL(0, rc);
  TEST_ASSERT_EQUAL_STRING("evdev keyboard", buf);
  close(fd);
}

static void test_evdev_caps_key(void) {
  int fd = open("/dev/input/event0", O_RDWR);
  TEST_ASSERT_GREATER_OR_EQUAL(0, fd);
  uint8_t buf[48] = {};
  int rc = ioctl(fd, EVIOCGBIT(EV_KEY, 48), buf);
  TEST_ASSERT_EQUAL(0, rc);
  // KEY_A = 30 → bit 30 in KEY bitmap → byte 3 (bits 24-31), bit offset 6
  // Actually KEY_A is in ev_key bitmap: bit 30 means byte[30/8]=byte[3],
  // bit 30%8=6
  TEST_ASSERT_BIT_HIGH(6, buf[3]);
  close(fd);
}

static void test_evdev_caps_abs(void) {
  int fd = open("/dev/input/event0", O_RDWR);
  TEST_ASSERT_GREATER_OR_EQUAL(0, fd);
  uint8_t buf[8] = {};
  int rc = ioctl(fd, EVIOCGBIT(EV_ABS, 8), buf);
  TEST_ASSERT_EQUAL(0, rc);
  TEST_ASSERT_EQUAL(0, buf[0]);
  close(fd);
}

static void test_evdev_prop(void) {
  int fd = open("/dev/input/event0", O_RDWR);
  TEST_ASSERT_GREATER_OR_EQUAL(0, fd);
  uint32_t buf = 0;
  int rc = ioctl(fd, EVIOCGPROP(4), &buf);
  TEST_ASSERT_EQUAL(0, rc);
  TEST_ASSERT_EQUAL(0, buf);
  close(fd);
}

static void test_evdev_abs_info(void) {
  int fd = open("/dev/input/event0", O_RDWR);
  TEST_ASSERT_GREATER_OR_EQUAL(0, fd);
  struct input_absinfo ai = {};
  int rc = ioctl(fd, EVIOCGABS(ABS_X), &ai);
  // evdev.cc has no ABS axes → -1/ENOSYS (POSIX errno convention, matching
  // test_ioctl/test_dev_vfs: rc==-1, errno==ENOSYS).
  TEST_ASSERT_EQUAL(-1, rc);
  TEST_ASSERT_EQUAL_INT(ENOSYS, errno);
  close(fd);
}

static void test_evdev_grab(void) {
  int fd = open("/dev/input/event0", O_RDWR);
  TEST_ASSERT_GREATER_OR_EQUAL(0, fd);
  // EVIOCGRAB is _IOW('E',0x90,int): the libc ioctl wrapper and the kernel
  // REQ proxy treat arg as a pointer to the int payload (copied into
  // msg->data+4, which evdev.cc reads). Pass &val, not a bare integer — a
  // bare 1 would be dereferenced as a pointer by the wrapper's copy-in.
  int grab_on = 1;
  int rc = ioctl(fd, EVIOCGRAB, &grab_on);
  TEST_ASSERT_EQUAL(0, rc);
  int grab_off = 0;
  rc = ioctl(fd, EVIOCGRAB, &grab_off);
  TEST_ASSERT_EQUAL(0, rc);
  close(fd);
}

static void test_evdev_grab_exclusive(void) {
  int fd = open("/dev/input/event0", O_RDWR);
  TEST_ASSERT_GREATER_OR_EQUAL(0, fd);
  int grab_on = 1;
  int rc = ioctl(fd, EVIOCGRAB, &grab_on);
  TEST_ASSERT_EQUAL(0, rc);

  pid_t child = fork();
  if (child == 0) {
    int fd2 = open("/dev/input/event0", O_RDWR);
    if (fd2 < 0) {
      _exit(1);
    }
    int32_t v = 0;
    int rc2 = ioctl(fd2, EVIOCGVERSION, &v);
    close(fd2);
    // libc ioctl translates the driver's -EBUSY reply into rc==-1 +
    // errno==EBUSY (POSIX convention, see test_ioctl/test_dev_vfs).
    _exit((rc2 == -1 && errno == EBUSY) ? 0 : 2);
  } else {
    int status;
    waitpid(child, &status, 0);
    TEST_ASSERT_EQUAL(0, status);
    int grab_off = 0;
    ioctl(fd, EVIOCGRAB, &grab_off);
    close(fd);
  }
}

static void test_evdev_routing(void) {
  int fd = open("/dev/input/event1", O_RDWR);
  TEST_ASSERT_GREATER_OR_EQUAL(0, fd);
  int32_t v = 0;
  int rc = ioctl(fd, EVIOCGVERSION, &v);
  TEST_ASSERT_EQUAL(0, rc);
  TEST_ASSERT_EQUAL(EV_VERSION, v);
  close(fd);
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();

  wait_dev_ready("/dev/input/event0");

  RUN_TEST(test_evdev_version);
  RUN_TEST(test_evdev_id);
  RUN_TEST(test_evdev_name);
  RUN_TEST(test_evdev_caps_key);
  RUN_TEST(test_evdev_caps_abs);
  RUN_TEST(test_evdev_prop);
  RUN_TEST(test_evdev_abs_info);
  RUN_TEST(test_evdev_grab);
  RUN_TEST(test_evdev_grab_exclusive);
  RUN_TEST(test_evdev_routing);

  return UNITY_END();
}

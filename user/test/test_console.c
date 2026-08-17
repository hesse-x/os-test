/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// /dev/console device test. console aliases the serial port (same serial_ops,
// registered as a second devtmpfs node in serial.c), so it is an output-only
// char device: open/write/poll/stat succeed, read is unsupported (mirrors
// /dev/serial after RX removal). This guards the alias node itself — the
// musl LOG_CONS fallback (third_party/musl src/misc/syslog.c) opens
// /dev/console, so a missing node silently breaks that path.

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

// Node exists and opens as a char device.
void test_console_open(void) {
  int fd = open("/dev/console", O_RDWR);
  TEST_ASSERT_TRUE(fd >= 0);
  close(fd);
}

// stat reports a character device (S_ISCHR), not a regular file.
void test_console_stat_is_chardev(void) {
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat("/dev/console", &st));
  TEST_ASSERT_TRUE(S_ISCHR(st.st_mode));
}

// Write via serial_ops.write reaches the serial TX path and returns count.
void test_console_write(void) {
  int fd = open("/dev/console", O_RDWR);
  TEST_ASSERT_TRUE(fd >= 0);
  ssize_t w = write(fd, "console-probe\n", 14);
  TEST_ASSERT_EQUAL_INT(14, (int)w);
  close(fd);
}

// Read is unsupported (serial RX removed) → -1/ENOSYS, same as /dev/serial.
void test_console_read_unsupported(void) {
  int fd = open("/dev/console", O_RDWR | O_NONBLOCK);
  TEST_ASSERT_TRUE(fd >= 0);
  char buf[1];
  ssize_t r = read(fd, buf, 1);
  TEST_ASSERT_EQUAL_INT(-1, (int)r);
  TEST_ASSERT_EQUAL_INT(ENOSYS, errno);
  close(fd);
}

// poll reports POLLOUT always ready (output-only device).
void test_console_poll(void) {
  int fd = open("/dev/console", O_RDWR);
  TEST_ASSERT_TRUE(fd >= 0);
  struct pollfd pfd = {fd, POLLIN | POLLOUT, 0};
  int r = poll(&pfd, 1, 100);
  TEST_ASSERT_TRUE(r > 0);
  TEST_ASSERT_TRUE(pfd.revents & POLLOUT);
  close(fd);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_console_open);
  RUN_TEST(test_console_stat_is_chardev);
  RUN_TEST(test_console_write);
  RUN_TEST(test_console_read_unsupported);
  RUN_TEST(test_console_poll);
  return UNITY_END();
}

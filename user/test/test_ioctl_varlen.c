/*
 * Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 *
 * test_ioctl_varlen — variable-length ioctl via user-space driver proxy.
 * Fork: parent = driver (sys_dev_create + recv loop), child = client (ioctl).
 * Covers: RECV_REQ inline (<=48B), RECV_IOCTL varlen (>48B), boundary 48/49,
 *         65536 cap, ETIMEDOUT, arg layout (pure data, no result prefix).
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/ioctl.h>
#include <sys/process.h>
#include <sys/wait.h>
#include <syscall.h>
#include <unistd.h>
#include <unity.h>
#include <xos/errno.h>
#include <xos/ioctl.h>

void setUp(void) {}
void tearDown(void) {}

#define TEST_DEV "test_varlen"

#define VARLEN_INLINE_48  _IOWR('V', 1, char[48])
#define VARLEN_VARLEN_49  _IOWR('V', 2, char[49])
#define VARLEN_VARLEN_96  _IOWR('V', 3, char[96])
#define VARLEN_VARLEN_256 _IOR('V', 4, char[256])
#define VARLEN_TOO_BIG    _IOWR('V', 5, char[65537])

static void driver_handle_req(struct recv_msg *msg, uint8_t *data_buf) {
  (void)data_buf;
  if (msg->type == RECV_REQ) {
    uint32_t cmd = *(uint32_t *)msg->data;
    uint16_t sz = _IOC_SIZE(cmd);
    static uint8_t reply[256];
    for (uint16_t i = 0; i < sz && i < sizeof(reply); i++)
      reply[i] = 0xAA;
    sys_resp(reply, sz, 0);
  } else if (msg->type == RECV_IOCTL) {
    uint16_t sz = msg->ioctl.arg_size;
    static uint8_t reply[65536];
    for (uint32_t i = 0; i < sz && i < sizeof(reply); i++)
      reply[i] = 0xBB;
    sys_resp(reply, sz, 0);
  }
}

static void run_driver(void) {
  int r = sys_dev_create(TEST_DEV, -1, 0);
  if (r < 0 && errno != EEXIST) {
    printf("driver: sys_dev_create failed errno=%d\n", errno);
    _exit(1);
  }
  for (int i = 0; i < 200; i++) {
    struct recv_msg msg;
    uint8_t data_buf[65536];
    int rc = recv(&msg, data_buf, sizeof(data_buf), 5000);
    if (rc < 0) {
      if (errno == EINTR)
        continue;
      printf("driver: recv errno=%d\n", errno);
      _exit(1);
    }
    driver_handle_req(&msg, data_buf);
  }
  _exit(0);
}

void test_varlen_96(void) {
  pid_t driver_pid = fork();
  if (driver_pid == 0) {
    run_driver();
    return;
  }
  usleep(200000);
  int fd = open("/dev/" TEST_DEV, O_RDWR);
  if (fd < 0) {
    waitpid(driver_pid, NULL, 0);
    TEST_ASSERT_TRUE(0 && "cannot open test device");
    return;
  }
  uint8_t arg[96];
  memset(arg, 0, sizeof(arg));
  int r = ioctl(fd, VARLEN_VARLEN_96, arg);
  TEST_ASSERT_EQUAL_INT(0, r);
  for (int i = 0; i < 96; i++)
    TEST_ASSERT_EQUAL_INT(0xBB, arg[i]);
  close(fd);
  waitpid(driver_pid, NULL, 0);
}

void test_boundary_48_49(void) {
  pid_t driver_pid = fork();
  if (driver_pid == 0) {
    run_driver();
    return;
  }
  usleep(200000);
  int fd = open("/dev/" TEST_DEV, O_RDWR);
  TEST_ASSERT_TRUE(fd >= 0);

  uint8_t a48[48];
  memset(a48, 0, sizeof(a48));
  TEST_ASSERT_EQUAL_INT(0, ioctl(fd, VARLEN_INLINE_48, a48));
  for (int i = 0; i < 48; i++)
    TEST_ASSERT_EQUAL_INT(0xAA, a48[i]);

  close(fd);
  waitpid(driver_pid, NULL, 0);

  driver_pid = fork();
  if (driver_pid == 0) {
    run_driver();
    return;
  }
  usleep(200000);
  fd = open("/dev/" TEST_DEV, O_RDWR);
  TEST_ASSERT_TRUE(fd >= 0);
  uint8_t a49[49];
  memset(a49, 0, sizeof(a49));
  TEST_ASSERT_EQUAL_INT(0, ioctl(fd, VARLEN_VARLEN_49, a49));
  for (int i = 0; i < 49; i++)
    TEST_ASSERT_EQUAL_INT(0xBB, a49[i]);
  close(fd);
  waitpid(driver_pid, NULL, 0);
}

void test_varlen_256_ior(void) {
  pid_t driver_pid = fork();
  if (driver_pid == 0) {
    run_driver();
    return;
  }
  usleep(200000);
  int fd = open("/dev/" TEST_DEV, O_RDWR);
  TEST_ASSERT_TRUE(fd >= 0);
  uint8_t arg[256];
  memset(arg, 0, sizeof(arg));
  TEST_ASSERT_EQUAL_INT(0, ioctl(fd, VARLEN_VARLEN_256, arg));
  for (int i = 0; i < 256; i++)
    TEST_ASSERT_EQUAL_INT(0xBB, arg[i]);
  close(fd);
  waitpid(driver_pid, NULL, 0);
}

void test_too_big(void) {
  pid_t driver_pid = fork();
  if (driver_pid == 0) {
    run_driver();
    return;
  }
  usleep(200000);
  int fd = open("/dev/" TEST_DEV, O_RDWR);
  TEST_ASSERT_TRUE(fd >= 0);
  uint8_t arg[65537];
  int r = ioctl(fd, VARLEN_TOO_BIG, arg);
  TEST_ASSERT_EQUAL_INT(-1, r);
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
  close(fd);
  waitpid(driver_pid, NULL, 0);
}

void test_timeout(void) {
  pid_t driver_pid = fork();
  if (driver_pid == 0) {
    sys_dev_create(TEST_DEV, -1, 0);
    sleep(5);
    _exit(0);
  }
  usleep(200000);
  int fd = open("/dev/" TEST_DEV, O_RDWR);
  TEST_ASSERT_TRUE(fd >= 0);
  uint8_t arg[96];
  int r = ioctl(fd, VARLEN_VARLEN_96, arg);
  TEST_ASSERT_EQUAL_INT(-1, r);
  TEST_ASSERT_EQUAL_INT(ETIMEDOUT, errno);
  close(fd);
  waitpid(driver_pid, NULL, 0);
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();
  RUN_TEST(test_varlen_96);
  RUN_TEST(test_boundary_48_49);
  RUN_TEST(test_varlen_256_ior);
  RUN_TEST(test_too_big);
  RUN_TEST(test_timeout);
  return UNITY_END();
}

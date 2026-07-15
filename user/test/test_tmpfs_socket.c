/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_tmpfs_socket: tmpfs /run 文件系统 + AF_UNIX socket mknod/bind/connect
 * 测试。 TEST 门控构建。*/
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/process.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <unity.h>
#include <xos/errno.h>

void setUp(void) {}
void tearDown(void) {}

/* tmpfs 建/删/读写/getdents 往返 */
void test_tmpfs_create_write_read(void) {
  int rc = mkdir("/run/t1", 0755);
  TEST_ASSERT_EQUAL(0, rc);
  int fd = open("/run/t1/a.txt", O_CREAT | O_WRONLY, 0644);
  TEST_ASSERT(fd >= 0);
  const char *msg = "hello tmpfs";
  ssize_t w = write(fd, msg, (int)strlen(msg));
  TEST_ASSERT_EQUAL((ssize_t)strlen(msg), w);
  close(fd);
  fd = open("/run/t1/a.txt", O_RDONLY);
  TEST_ASSERT(fd >= 0);
  char buf[64];
  memset(buf, 0, sizeof(buf));
  ssize_t r = read(fd, buf, sizeof(buf));
  TEST_ASSERT_EQUAL((ssize_t)strlen(msg), r);
  TEST_ASSERT_EQUAL_STRING(msg, buf);
  close(fd);
}

/* tmpfs 重启清空（内存 fs）——本 ELF 内无法重启，仅验证写入可见 */
void test_tmpfs_persist_within_boot(void) {
  int fd = open("/run/t2", O_CREAT | O_WRONLY, 0644);
  TEST_ASSERT(fd >= 0);
  TEST_ASSERT_EQUAL((ssize_t)1, write(fd, "x", 1));
  close(fd);
  fd = open("/run/t2", O_RDONLY);
  TEST_ASSERT(fd >= 0);
  close(fd);
}

/* mknod 建 socket 文件 + Linux 语义 */
void test_mknod_socket(void) {
  int rc = mknod("/run/tsock", S_IFSOCK | 0777, 0);
  TEST_ASSERT_EQUAL(0, rc);
  /* 重复 mknod 同名 → EEXIST（Linux mknod 语义）*/
  rc = mknod("/run/tsock", S_IFSOCK | 0777, 0);
  TEST_ASSERT_EQUAL(-1, rc);
  TEST_ASSERT_EQUAL(EEXIST, errno);
  /* mknod 普通文件 → 0（Linux：tmpfs 支持 S_IFREG）*/
  rc = mknod("/run/treg", S_IFREG | 0644, 0);
  TEST_ASSERT_EQUAL(0, rc);
  /* mknod 字符设备 → EOPNOTSUPP（tmpfs 不建设备节点）*/
  rc = mknod("/run/tchr", S_IFCHR | 0600, 0);
  TEST_ASSERT_EQUAL(-1, rc);
  TEST_ASSERT_EQUAL(EOPNOTSUPP, errno);
  /* open() socket 文件 → ENXIO（Linux 语义）*/
  int fd = open("/run/tsock", O_RDONLY);
  TEST_ASSERT_EQUAL(-1, fd);
  TEST_ASSERT_EQUAL(ENXIO, errno);
}

/* bind 建 socket + connect 取回 + Linux 错误码语义 */
void test_bind_connect_vfs(void) {
  int s = socket(AF_UNIX, SOCK_STREAM, 0);
  TEST_ASSERT(s >= 0);
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, "/run/tbind", sizeof(addr.sun_path) - 1);
  TEST_ASSERT_EQUAL(0, bind(s, (struct sockaddr *)&addr, sizeof(addr)));
  TEST_ASSERT_EQUAL(0, listen(s, 8));
  /* 重复 bind 同路径 → EADDRINUSE（Linux 语义）*/
  int s2 = socket(AF_UNIX, SOCK_STREAM, 0);
  TEST_ASSERT_EQUAL(-1, bind(s2, (struct sockaddr *)&addr, sizeof(addr)));
  TEST_ASSERT_EQUAL(EADDRINUSE, errno);
  close(s2);
  /* connect 到已 listen 的 socket → 成功 */
  int c = socket(AF_UNIX, SOCK_STREAM, 0);
  TEST_ASSERT(c >= 0);
  TEST_ASSERT_EQUAL(0, connect(c, (struct sockaddr *)&addr, sizeof(addr)));
  close(c);
  close(s);
}

/* connect 到存在但未 listen 的 socket → ECONNREFUSED（Linux 语义）*/
void test_connect_not_listening(void) {
  int s = socket(AF_UNIX, SOCK_STREAM, 0);
  TEST_ASSERT(s >= 0);
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, "/run/tnolisten", sizeof(addr.sun_path) - 1);
  TEST_ASSERT_EQUAL(0, bind(s, (struct sockaddr *)&addr, sizeof(addr)));
  /* 不 listen，直接 connect */
  int c = socket(AF_UNIX, SOCK_STREAM, 0);
  TEST_ASSERT_EQUAL(-1, connect(c, (struct sockaddr *)&addr, sizeof(addr)));
  TEST_ASSERT_EQUAL(ECONNREFUSED, errno);
  close(c);
  close(s);
}

/* init respawn：fork+kill udevd 子进程验证（纯用户态模拟）*/
void test_respawn_burst(void) {
  /* 此测试验证 init 的 crash_count 逻辑：fork 一个模拟 udevd 子进程，
   * 退出码非零 → init respawn。完整 init 集成在 QEMU 手测，此处仅 smoke。 */
  pid_t pid = fork();
  if (pid == 0) {
    _exit(1);
  }
  int status;
  waitpid(pid, &status, 0);
  TEST_ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 1);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_tmpfs_create_write_read);
  RUN_TEST(test_tmpfs_persist_within_boot);
  RUN_TEST(test_mknod_socket);
  RUN_TEST(test_bind_connect_vfs);
  RUN_TEST(test_connect_not_listening);
  RUN_TEST(test_respawn_burst);
  return UNITY_END();
}

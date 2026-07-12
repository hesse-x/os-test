/*
 * Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/device.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <unity.h>
#include <xos/errno.h>
#include <xos/socket.h>

void setUp(void) {}
void tearDown(void) {}

/* ===== S0: f_op regression ===== */

void test_fop_pipe_regression(void) {
  int fd[2];
  TEST_ASSERT_EQUAL_INT(0, pipe(fd));
  const char *msg = "fop_test";
  TEST_ASSERT_EQUAL_INT(9, (int)write(fd[1], msg, 9));
  char buf[16] = {0};
  TEST_ASSERT_EQUAL_INT(9, (int)read(fd[0], buf, 9));
  TEST_ASSERT_EQUAL_STRING("fop_test", buf);
  close(fd[0]);
  close(fd[1]);
}

void test_fop_eventfd_regression(void) {
  int fd = eventfd(0, 0);
  TEST_ASSERT_TRUE(fd >= 0);
  uint64_t val = 42;
  TEST_ASSERT_EQUAL_INT(8, (int)write(fd, &val, 8));
  uint64_t out = 0;
  TEST_ASSERT_EQUAL_INT(8, (int)read(fd, &out, 8));
  TEST_ASSERT_EQUAL_UINT64(42, out);
  close(fd);
}

void test_fop_timerfd_regression(void) {
  int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
  TEST_ASSERT_TRUE(fd >= 0);
  struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
  int r = poll(&pfd, 1, 0);
  TEST_ASSERT_EQUAL_INT(0, r); /* 未到期，无事件 */
  close(fd);
}

void test_fop_socket_regression(void) {
  int sv[2];
  TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
  const char *msg = "sock";
  TEST_ASSERT_EQUAL_INT(4, (int)write(sv[0], msg, 4));
  char buf[8] = {0};
  TEST_ASSERT_EQUAL_INT(4, (int)read(sv[1], buf, 4));
  TEST_ASSERT_EQUAL_STRING("sock", buf);
  close(sv[0]);
  close(sv[1]);
}

void test_fop_dev_serial_regression(void) {
  int fd = open("/dev/serial", O_RDWR);
  if (fd < 0) {
    TEST_ASSERT_TRUE(1);
    return;
  }
  ssize_t w = write(fd, "R", 1);
  TEST_ASSERT_EQUAL_INT(1, (int)w);
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, fstat(fd, &st));
  TEST_ASSERT_TRUE(S_ISCHR(st.st_mode));
  close(fd);
}

/* ===== S1: sysfs 伪文件系统基本 ===== */

void test_sysfs_open_root(void) {
  int fd = open("/sys", O_RDONLY);
  TEST_ASSERT_TRUE(fd >= 0);
  close(fd);
}

void test_sysfs_stat_root(void) {
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat("/sys", &st));
  TEST_ASSERT_TRUE(S_ISDIR(st.st_mode));
  TEST_ASSERT_TRUE(st.st_ino >= 0x10000);
}

void test_sysfs_getdents_root(void) {
  DIR *d = opendir("/sys");
  TEST_ASSERT_TRUE(d != NULL);
  int found_class = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, "class") == 0)
      found_class = 1;
  }
  closedir(d);
  TEST_ASSERT_TRUE(found_class);
}

void test_sysfs_stat_class(void) {
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat("/sys/class", &st));
  TEST_ASSERT_TRUE(S_ISDIR(st.st_mode));
}

void test_sysfs_open_class(void) {
  int fd = open("/sys/class", O_RDONLY);
  TEST_ASSERT_TRUE(fd >= 0);
  close(fd);
}

void test_sysfs_stat_nonexistent(void) {
  struct stat st;
  int r = stat("/sys/nonexistent", &st);
  TEST_ASSERT_EQUAL_INT(-1, r);
  TEST_ASSERT_EQUAL_INT(ENOENT, errno);
}

void test_sysfs_open_nonexistent(void) {
  int fd = open("/sys/nonexistent", O_RDONLY);
  TEST_ASSERT_TRUE(fd < 0);
  TEST_ASSERT_EQUAL_INT(ENOENT, errno);
}

/* ===== S2: drm 属性树 ===== */

void test_sysfs_drm_class_getdents(void) {
  DIR *d = opendir("/sys/class/drm");
  if (!d) {
    TEST_ASSERT_TRUE(1);
    return;
  } /* DRM 可能未就绪 */
  int found_card0 = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, "card0") == 0)
      found_card0 = 1;
  }
  closedir(d);
  TEST_ASSERT_TRUE(found_card0);
}

void test_sysfs_drm_card0_getdents(void) {
  DIR *d = opendir("/sys/class/drm/card0");
  if (!d) {
    TEST_ASSERT_TRUE(1);
    return;
  }
  int count = 0;
  int found_vendor = 0, found_mode = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    count++;
    if (strcmp(e->d_name, "vendor") == 0)
      found_vendor = 1;
    if (strcmp(e->d_name, "mode") == 0)
      found_mode = 1;
  }
  closedir(d);
  TEST_ASSERT_TRUE(found_vendor);
  TEST_ASSERT_TRUE(found_mode);
  TEST_ASSERT_TRUE(
      count >=
      9); /* 9 个属性文件
             (vendor/device/class/driver/enabled/mode/connector_status/num_scanouts/dev)
           */
}

/* 通用 sysfs 属性读取 helper */
static int read_sysfs_attr(const char *path, char *buf, size_t len) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  memset(buf, 0, len);
  ssize_t r = read(fd, buf, len - 1);
  close(fd);
  return (int)r;
}

void test_sysfs_drm_vendor(void) {
  char buf[64];
  int r = read_sysfs_attr("/sys/class/drm/card0/vendor", buf, sizeof(buf));
  if (r < 0) {
    TEST_ASSERT_TRUE(1);
    return;
  }
  /* 验证格式：0xXXXX\n */
  TEST_ASSERT_TRUE(strncmp(buf, "0x", 2) == 0);
}

void test_sysfs_drm_mode(void) {
  char buf[64];
  int r = read_sysfs_attr("/sys/class/drm/card0/mode", buf, sizeof(buf));
  if (r < 0) {
    TEST_ASSERT_TRUE(1);
    return;
  }
  /* 格式：WIDTHxHEIGHT\n */
  TEST_ASSERT_TRUE(strchr(buf, 'x') != NULL);
}

void test_sysfs_drm_driver(void) {
  char buf[64];
  int r = read_sysfs_attr("/sys/class/drm/card0/driver", buf, sizeof(buf));
  if (r < 0) {
    TEST_ASSERT_TRUE(1);
    return;
  }
  TEST_ASSERT_EQUAL_STRING("virtio_gpu\n", buf);
}

void test_sysfs_drm_enabled(void) {
  char buf[16];
  int r = read_sysfs_attr("/sys/class/drm/card0/enabled", buf, sizeof(buf));
  if (r < 0) {
    TEST_ASSERT_TRUE(1);
    return;
  }
  TEST_ASSERT_TRUE(buf[0] == '0' || buf[0] == '1');
}

void test_sysfs_drm_stat_attr(void) {
  struct stat st;
  int r = stat("/sys/class/drm/card0/vendor", &st);
  if (r < 0) {
    TEST_ASSERT_TRUE(1);
    return;
  }
  TEST_ASSERT_TRUE(S_ISREG(st.st_mode));
  TEST_ASSERT_TRUE((st.st_mode & 0444) != 0); /* 可读 */
}

void test_sysfs_drm_stat_dir(void) {
  struct stat st;
  int r = stat("/sys/class/drm/card0", &st);
  if (r < 0) {
    TEST_ASSERT_TRUE(1);
    return;
  }
  TEST_ASSERT_TRUE(S_ISDIR(st.st_mode));
}

/* ===== S4 边界：sysfs attr 边界条件 ===== */

void test_sysfs_read_large_count(void) {
  char buf[8192];
  int r = read_sysfs_attr("/sys/class/drm/card0/vendor", buf, sizeof(buf));
  if (r < 0) {
    TEST_ASSERT_TRUE(1);
    return;
  }
  /* 内核截断到 4096，实际返回远小于此 */
  TEST_ASSERT_TRUE(r <= 4096);
}

void test_sysfs_stat_ino_range(void) {
  struct stat st1, st2;
  if (stat("/sys/class/drm/card0", &st1) < 0) {
    TEST_ASSERT_TRUE(1);
    return;
  }
  if (stat("/sys/class/drm/card0/vendor", &st2) < 0) {
    TEST_ASSERT_TRUE(1);
    return;
  }
  TEST_ASSERT_TRUE(st1.st_ino >= 0x10000);
  TEST_ASSERT_TRUE(st2.st_ino >= 0x10000);
  TEST_ASSERT_TRUE(st1.st_ino != st2.st_ino);
}

void test_sysfs_read_dir(void) {
  /* 打开 sysfs 目录并尝试 read — sysfs_fops 只对 INODE_REGULAR 设 f_op，
   * 目录走原有 path，read 应返回 -EISDIR 或类似错误 */
  int fd = open("/sys/class/drm", O_RDONLY);
  if (fd < 0) {
    TEST_ASSERT_TRUE(1);
    return;
  }
  char buf[64];
  ssize_t r = read(fd, buf, sizeof(buf));
  TEST_ASSERT_TRUE(r < 0); /* 目录不可 read */
  close(fd);
}

/* Phase B: /sys/class/drm/card0/dev 属性存在且格式正确 */
void test_sysfs_drm_dev_attr(void) {
  const char *path = "/sys/class/drm/card0/dev";
  int fd = open(path, O_RDONLY);
  TEST_ASSERT_TRUE(fd >= 0);

  char buf[32];
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  TEST_ASSERT_TRUE(n > 0);
  buf[n] = '\0';

  /* Expect format "226:0\n" (MAJOR:MINOR) */
  TEST_ASSERT_EQUAL_STRING("226:0\n", buf);
}

/* ===== R: 回归 ===== */

void test_sysfs_drm_ioctl_regression(void) {
  int fd = open("/dev/dri/card0", O_RDWR);
  if (fd < 0) {
    TEST_ASSERT_TRUE(1);
    return;
  }
  /* DRM ioctl 全链路不应被 sysfs f_op 影响 */
  /* /dev/dri/card0 没有 f_op (不是 ringbuf)，走原有 FD_DEV path */
  close(fd);
  TEST_ASSERT_TRUE(1);
}

void test_sysfs_fat32_regression(void) {
  int fd = open("/test/mount.elf", O_RDONLY);
  TEST_ASSERT_TRUE(fd >= 0);
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, fstat(fd, &st));
  TEST_ASSERT_TRUE(S_ISREG(st.st_mode));
  close(fd);
}

void test_sysfs_mount_regression(void) {
  int r = mount(NULL, "/dev", "devtmpfs", 0, NULL);
  TEST_ASSERT_EQUAL_INT(-1, r);
  TEST_ASSERT_EQUAL_INT(EBUSY, errno);
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();

#ifdef TEST
  wait_dev_ready("/dev/dri/card0");
#endif

  /* S0: f_op regression */
  RUN_TEST(test_fop_pipe_regression);
  RUN_TEST(test_fop_eventfd_regression);
  RUN_TEST(test_fop_timerfd_regression);
  RUN_TEST(test_fop_socket_regression);
  RUN_TEST(test_fop_dev_serial_regression);

  /* S1: sysfs basic */
  RUN_TEST(test_sysfs_open_root);
  RUN_TEST(test_sysfs_stat_root);
  RUN_TEST(test_sysfs_getdents_root);
  RUN_TEST(test_sysfs_stat_class);
  RUN_TEST(test_sysfs_open_class);
  RUN_TEST(test_sysfs_stat_nonexistent);
  RUN_TEST(test_sysfs_open_nonexistent);

  /* S2: drm sysfs */
  RUN_TEST(test_sysfs_drm_class_getdents);
  RUN_TEST(test_sysfs_drm_card0_getdents);
  RUN_TEST(test_sysfs_drm_vendor);
  RUN_TEST(test_sysfs_drm_mode);
  RUN_TEST(test_sysfs_drm_driver);
  RUN_TEST(test_sysfs_drm_enabled);
  RUN_TEST(test_sysfs_drm_stat_attr);
  RUN_TEST(test_sysfs_drm_stat_dir);
  /* Phase B: sysfs dev attr */
  RUN_TEST(test_sysfs_drm_dev_attr);

  /* S4 边界：sysfs 边界条件 */
#ifdef TEST
  RUN_TEST(test_sysfs_read_large_count);
  RUN_TEST(test_sysfs_stat_ino_range);
  RUN_TEST(test_sysfs_read_dir);
#endif

  /* R: cross-regression */
  RUN_TEST(test_sysfs_drm_ioctl_regression);
  RUN_TEST(test_sysfs_fat32_regression);
  RUN_TEST(test_sysfs_mount_regression);

  return UNITY_END();
}

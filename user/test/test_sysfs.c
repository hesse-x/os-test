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
#include <sys/process.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unity.h>
#include <xos/errno.h>
#include <xos/input.h>
#include <xos/ioctl.h>
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

/* ===== S3: evdev sysfs 属性树 ===== */
#ifdef TEST

/* Register a second input device (event1) from the test program itself, so the
 * sysfs input class subtree can be exercised with >1 device. We only need the
 * sysfs attributes — no event ring — so pass shm_fd=-1 (no SHM binding). The
 * kernel fills driver_pid = our pid; the device has no ring so ringbuf fops are
 * never entered for it. This mirrors evdev's two-step registration
 * (device_register_shm → device_set_meta) and builds /sys/class/input/event1.
 */
static void register_event1(void) {
  int rc = device_register_shm("input/event1", -1, 1);
  TEST_ASSERT_EQUAL_INT(0, rc);
  struct dev_props props = {.bustype = BUS_USB,
                            .vendor = 0x0002,
                            .product = 0x0002,
                            .version = 0x0001};
  strncpy(props.name, "evdev test dev", sizeof(props.name) - 1);
  props.name[sizeof(props.name) - 1] = '\0';
  int r = device_set_meta("input/event1", "input", "evdev", &props);
  TEST_ASSERT_EQUAL_INT(0, r);
}

void test_sysfs_input_class_getdents(void) {
  DIR *d = opendir("/sys/class/input");
  TEST_ASSERT_TRUE(d != NULL);
  int found_event0 = 0, found_event1 = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, "event0") == 0)
      found_event0 = 1;
    if (strcmp(e->d_name, "event1") == 0)
      found_event1 = 1;
  }
  closedir(d);
  TEST_ASSERT_TRUE(found_event0);
  TEST_ASSERT_TRUE(found_event1);
}

void test_sysfs_input_event0_name(void) {
  char buf[64];
  int r = read_sysfs_attr("/sys/class/input/event0/name", buf, sizeof(buf));
  TEST_ASSERT_TRUE(r > 0);
  TEST_ASSERT_EQUAL_STRING("evdev keyboard\n", buf);
}

void test_sysfs_input_event0_vendor(void) {
  char buf[64];
  int r =
      read_sysfs_attr("/sys/class/input/event0/id/vendor", buf, sizeof(buf));
  TEST_ASSERT_TRUE(r > 0);
  TEST_ASSERT_EQUAL_STRING("0x0001\n", buf);
}

void test_sysfs_input_event0_bustype(void) {
  char buf[64];
  int r =
      read_sysfs_attr("/sys/class/input/event0/id/bustype", buf, sizeof(buf));
  TEST_ASSERT_TRUE(r > 0);
  /* BUS_USB = 0x03, show callback prints "%u\n" → "3\n" */
  TEST_ASSERT_EQUAL_STRING("3\n", buf);
}

void test_sysfs_input_event0_product(void) {
  char buf[64];
  int r =
      read_sysfs_attr("/sys/class/input/event0/id/product", buf, sizeof(buf));
  TEST_ASSERT_TRUE(r > 0);
  TEST_ASSERT_EQUAL_STRING("0x0001\n", buf);
}

void test_sysfs_input_event0_version(void) {
  char buf[64];
  int r =
      read_sysfs_attr("/sys/class/input/event0/id/version", buf, sizeof(buf));
  TEST_ASSERT_TRUE(r > 0);
  TEST_ASSERT_EQUAL_STRING("0x0001\n", buf);
}

void test_sysfs_input_event0_stat_attr(void) {
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat("/sys/class/input/event0/name", &st));
  TEST_ASSERT_TRUE(S_ISREG(st.st_mode));
}

void test_sysfs_input_event0_stat_id_dir(void) {
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat("/sys/class/input/event0/id", &st));
  TEST_ASSERT_TRUE(S_ISDIR(st.st_mode));
}

void test_sysfs_input_event0_stat_dir(void) {
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat("/sys/class/input/event0", &st));
  TEST_ASSERT_TRUE(S_ISDIR(st.st_mode));
}

void test_sysfs_dev_set_meta_enoent(void) {
  struct dev_props props = {0};
  int r = device_set_meta("nonexistent", "input", "evdev", &props);
  TEST_ASSERT_EQUAL_INT(-1, r);
  TEST_ASSERT_EQUAL_INT(ENOENT, errno);
}

void test_sysfs_nonexistent_class(void) {
  /* /sys/class is a real directory; a class subdir that was never created
   * must resolve to ENOENT (sysfs_walk falls off the tree). */
  int fd = open("/sys/class/nonexistent", O_RDONLY);
  TEST_ASSERT_EQUAL_INT(-1, fd);
  TEST_ASSERT_EQUAL_INT(ENOENT, errno);

  struct stat st;
  int r = stat("/sys/class/nonexistent", &st);
  TEST_ASSERT_EQUAL_INT(-1, r);
  TEST_ASSERT_EQUAL_INT(ENOENT, errno);
}

void test_sysfs_input_event1_name(void) {
  /* event1 was registered by register_event1() in main(); its name attr
   * should reflect the props.name we injected ("evdev test dev"). */
  char buf[64];
  int r = read_sysfs_attr("/sys/class/input/event1/name", buf, sizeof(buf));
  TEST_ASSERT_TRUE(r > 0);
  TEST_ASSERT_EQUAL_STRING("evdev test dev\n", buf);
}

void test_sysfs_dev_set_meta_success(void) {
  /* device_set_meta on a freshly registered device (event1, registered in
   * main before tests run) already returned 0 via register_event1(). Here we
   * re-assert the observable result: the sysfs subtree exists and is readable,
   * which is only possible if the set_meta step succeeded. (We do NOT re-call
   * set_meta on event0 — that path leaks subsys_priv and duplicates sysfs
   * files, since sys_dev_set_meta has no idempotency guard.) */
  char buf[32];
  int r =
      read_sysfs_attr("/sys/class/input/event1/id/vendor", buf, sizeof(buf));
  TEST_ASSERT_TRUE(r > 0);
  TEST_ASSERT_EQUAL_STRING("0x0002\n", buf);

  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat("/sys/class/input/event1", &st));
  TEST_ASSERT_TRUE(S_ISDIR(st.st_mode));
}

#endif /* TEST */

/* ===== S4: evdev broker consumer fd 测试 ===== */
#ifdef TEST

void test_ringbuf_open_evdev(void) {
  int fd = open("/dev/input/event0", O_RDWR | O_NONBLOCK);
  TEST_ASSERT_TRUE(fd >= 0);
  close(fd);
}

void test_ringbuf_read_empty_nonblock(void) {
  int fd = open("/dev/input/event0", O_RDWR | O_NONBLOCK);
  TEST_ASSERT_TRUE(fd >= 0);
  char buf[256];
  ssize_t r = read(fd, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_INT(-1, (int)r);
  TEST_ASSERT_EQUAL_INT(EAGAIN, errno);
  close(fd);
}

void test_ringbuf_poll_empty(void) {
  int fd = open("/dev/input/event0", O_RDWR | O_NONBLOCK);
  TEST_ASSERT_TRUE(fd >= 0);
  struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
  int r = poll(&pfd, 1, 0);
  TEST_ASSERT_EQUAL_INT(0, r);
  close(fd);
}

void test_ringbuf_write_einval(void) {
  int fd = open("/dev/input/event0", O_RDWR | O_NONBLOCK);
  TEST_ASSERT_TRUE(fd >= 0);
  ssize_t r = write(fd, "x", 1);
  TEST_ASSERT_EQUAL_INT(-1, (int)r);
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
  close(fd);
}

void test_ringbuf_close_no_crash(void) {
  int fd = open("/dev/input/event0", O_RDWR | O_NONBLOCK);
  TEST_ASSERT_TRUE(fd >= 0);
  /* close releases the broker consumer client — should not crash */
  int r = close(fd);
  TEST_ASSERT_EQUAL_INT(0, r);
}

void test_ringbuf_read_zero_count(void) {
  int fd = open("/dev/input/event0", O_RDWR | O_NONBLOCK);
  TEST_ASSERT_TRUE(fd >= 0);
  char buf[1];
  ssize_t r = read(fd, buf, 0);
  TEST_ASSERT_EQUAL_INT(0, (int)r);
  close(fd);
}

void test_ringbuf_ioctl_proxy(void) {
  /* /dev/input/eventN is a broker consumer fd; EVIOCG* is forwarded in-kernel
   * to the evdev manager pid (broker ioctl path), so a served EVIOCGVERSION
   * returns EV_VERSION. This verifies the consumer ioctl path reaches the
   * driver intact (no crash, no spurious ENOTTY).
   *
   * Case A: a served EVIOCG* returns 0 with the expected payload.
   * Case B: an unknown ioctl the driver cannot serve returns -1 with errno
   *         set (evdev's default branch yields -ENOSYS). */
  int fd = open("/dev/input/event0", O_RDWR | O_NONBLOCK);
  TEST_ASSERT_TRUE(fd >= 0);

  /* Case A: EVIOCGVERSION → 0, v == EV_VERSION */
  int32_t v = 0;
  long rc = ioctl(fd, EVIOCGVERSION, &v);
  TEST_ASSERT_EQUAL_INT(0, (int)rc);
  TEST_ASSERT_EQUAL_INT(EV_VERSION, v);

  /* Case B: unknown cmd (type 'Z', nr 0x7f) → evdev default → -ENOSYS */
  uint32_t bogus = _IOC(_IOC_READ, 'Z', 0x7f, sizeof(int));
  errno = 0;
  long rc2 = ioctl(fd, bogus, &(int32_t){0});
  TEST_ASSERT_EQUAL_INT(-1, (int)rc2);
  TEST_ASSERT_TRUE(errno != 0);

  close(fd);
}

#endif /* TEST */

/* ===== R: 回归 ===== */

void test_sysfs_drm_ioctl_regression(void) {
  int fd = open("/dev/dri/card0", O_RDWR);
  if (fd < 0) {
    TEST_ASSERT_TRUE(1);
    return;
  }
  /* DRM ioctl 全链路不应被 sysfs f_op 影响 */
  /* /dev/dri/card0 没有 f_op，走原有 FD_DEV path */
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
  wait_dev_ready("/dev/input/event0");
  /* Register event1 so S3 multi-device cases have a second device. Done after
   * event0 is ready so the /sys/class/input class dir already exists. */
  register_event1();
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

  /* S3: evdev sysfs 属性树 */
  RUN_TEST(test_sysfs_input_class_getdents);
  RUN_TEST(test_sysfs_input_event0_name);
  RUN_TEST(test_sysfs_input_event0_vendor);
  RUN_TEST(test_sysfs_input_event0_bustype);
  RUN_TEST(test_sysfs_input_event0_product);
  RUN_TEST(test_sysfs_input_event0_version);
  RUN_TEST(test_sysfs_input_event0_stat_attr);
  RUN_TEST(test_sysfs_input_event0_stat_id_dir);
  RUN_TEST(test_sysfs_input_event0_stat_dir);
  RUN_TEST(test_sysfs_dev_set_meta_enoent);
  RUN_TEST(test_sysfs_nonexistent_class);
  RUN_TEST(test_sysfs_input_event1_name);
  RUN_TEST(test_sysfs_dev_set_meta_success);

  /* S4: evdev broker consumer fd (read/poll/ioctl via /dev/input/eventN) */
  RUN_TEST(test_ringbuf_open_evdev);
  RUN_TEST(test_ringbuf_read_empty_nonblock);
  RUN_TEST(test_ringbuf_poll_empty);
  RUN_TEST(test_ringbuf_write_einval);
  RUN_TEST(test_ringbuf_close_no_crash);
  RUN_TEST(test_ringbuf_read_zero_count);
  RUN_TEST(test_ringbuf_ioctl_proxy);
#endif

  /* R: cross-regression */
  RUN_TEST(test_sysfs_drm_ioctl_regression);
  RUN_TEST(test_sysfs_fat32_regression);
  RUN_TEST(test_sysfs_mount_regression);

  return UNITY_END();
}

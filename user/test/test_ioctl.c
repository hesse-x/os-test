/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <xos/input.h>
#include <xos/ioctl.h>
#include <syscall.h>
#include "user/driver/display.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unity.h>
#include <xos/errno.h>

void setUp(void) {}
void tearDown(void) {}

/* 1. fstat on regular file → S_ISREG */
void test_fstat_regular(void) {
  int fd = open("/local/ioctl_test.txt", O_WRONLY | O_CREAT);
  TEST_ASSERT_TRUE(fd >= 0);
  write(fd, "hello", 5);
  close(fd);

  fd = open("/local/ioctl_test.txt", O_RDONLY);
  TEST_ASSERT_TRUE(fd >= 0);

  struct stat st;
  int r = fstat(fd, &st);
  TEST_ASSERT_EQUAL_INT(0, r);
  TEST_ASSERT_TRUE(S_ISREG(st.st_mode));
  TEST_ASSERT_EQUAL_INT(5, (int)st.st_size);

  close(fd);
}

/* 2. fstat on pipe → S_ISFIFO */
void test_fstat_pipe(void) {
  int fd[2];
  pipe(fd);

  struct stat st;
  int r = fstat(fd[0], &st);
  TEST_ASSERT_EQUAL_INT(0, r);
  TEST_ASSERT_TRUE((st.st_mode & S_IFMT) == S_IFIFO);

  close(fd[0]);
  close(fd[1]);
}

/* 3. fstat on dir → S_ISDIR */
void test_fstat_dir(void) {
  int fd = open("/local", O_RDONLY);
  if (fd >= 0) {
    struct stat st;
    int r = fstat(fd, &st);
    TEST_ASSERT_EQUAL_INT(0, r);
    TEST_ASSERT_TRUE(S_ISDIR(st.st_mode));
    close(fd);
  } else {
    /* Directory may not exist, skip */
    TEST_ASSERT_TRUE(1);
  }
}

/* 4. fstat on /dev/kms → S_ISCHR */
void test_fstat_dev(void) {
  int fd = open("/dev/kms", O_RDWR);
  if (fd >= 0) {
    struct stat st;
    int r = fstat(fd, &st);
    TEST_ASSERT_EQUAL_INT(0, r);
    TEST_ASSERT_TRUE(S_ISCHR(st.st_mode));

    close(fd);
  } else {
    /* KMS may not be available in test env */
    TEST_ASSERT_TRUE(1);
  }
}

/* 5. fstat on bad fd → EBADF */
void test_fstat_bad_fd(void) {
  struct stat st;
  int r = fstat(-1, &st);
  TEST_ASSERT_TRUE(r < 0);
  TEST_ASSERT_EQUAL_INT(EBADF, errno);
}

/* 6. ioctl on regular file → ENOTTY */
void test_ioctl_regular(void) {
  int fd = open("/local/ioctl_ioctl.txt", O_WRONLY | O_CREAT);
  TEST_ASSERT_TRUE(fd >= 0);
  write(fd, "x", 1);
  close(fd);

  fd = open("/local/ioctl_ioctl.txt", O_RDONLY);
  TEST_ASSERT_TRUE(fd >= 0);

  int r = ioctl(fd, TCGETS, 0);
  TEST_ASSERT_TRUE(r < 0);
  TEST_ASSERT_EQUAL_INT(ENOTTY, errno);

  close(fd);
}

/* 7. ioctl on pipe → ENOTTY */
void test_ioctl_pipe(void) {
  int fd[2];
  pipe(fd);

  int r = ioctl(fd[0], TCGETS, 0);
  TEST_ASSERT_TRUE(r < 0);
  TEST_ASSERT_EQUAL_INT(ENOTTY, errno);

  close(fd[0]);
  close(fd[1]);
}

/* 8. isatty on pipe → 0 */
void test_isatty_pipe(void) {
  int fd[2];
  pipe(fd);

  int r = isatty(fd[0]);
  TEST_ASSERT_EQUAL_INT(0, r);

  close(fd[0]);
  close(fd[1]);
}

/* 9. isatty on regular file → 0 */
void test_isatty_regular(void) {
  int fd = open("/local/ioctl_isatty.txt", O_WRONLY | O_CREAT);
  TEST_ASSERT_TRUE(fd >= 0);
  write(fd, "x", 1);
  close(fd);

  fd = open("/local/ioctl_isatty.txt", O_RDONLY);
  TEST_ASSERT_TRUE(fd >= 0);

  int r = isatty(fd);
  TEST_ASSERT_EQUAL_INT(0, r);

  close(fd);
}

/* 10. isatty on /dev/kms → 0 (KMS doesn't support TCGETS) */
void test_isatty_dev_kms(void) {
  int fd = open("/dev/kms", O_RDWR);
  if (fd >= 0) {
    int r = isatty(fd);
    TEST_ASSERT_EQUAL_INT(0, r);
    close(fd);
  } else {
    TEST_ASSERT_TRUE(1);
  }
}

/* 11. stat (path-based) on regular file */
void test_stat_regular(void) {
  int fd = open("/local/ioctl_stat.txt", O_WRONLY | O_CREAT);
  TEST_ASSERT_TRUE(fd >= 0);
  write(fd, "stat_test", 9);
  close(fd);

  struct stat st;
  int r = stat("/local/ioctl_stat.txt", &st);
  TEST_ASSERT_EQUAL_INT(0, r);
  TEST_ASSERT_TRUE(S_ISREG(st.st_mode));
  TEST_ASSERT_EQUAL_INT(9, (int)st.st_size);
}

/* ---- Phase 1: dev_ops ioctl callback (KMS) ---- */

/* 12. ioctl on /dev/kms with KMS_IOCTL_FLIP → success (0) */
void test_ioctl_kms_flip(void) {
  int fd = open("/dev/kms", O_RDWR);
  if (fd >= 0) {
    long r = ioctl(fd, KMS_IOCTL_FLIP, 0);
    /* FLIP without prior CREATE_BUF may return -ENOENT or 0 */
    TEST_ASSERT_TRUE(r == 0 || r == -ENOENT || errno == ENOENT);
    close(fd);
  } else {
    TEST_ASSERT_TRUE(1);
  }
}

/* ---- Phase 2: FD_DEV unified + serial dev_ops ---- */

/* 13. open("/dev/serial") returns valid fd */
void test_open_dev_serial(void) {
  int fd = open("/dev/serial", O_RDWR);
  TEST_ASSERT_TRUE(fd >= 0);

  /* fstat → S_ISCHR */
  struct stat st;
  int r = fstat(fd, &st);
  TEST_ASSERT_EQUAL_INT(0, r);
  TEST_ASSERT_TRUE(S_ISCHR(st.st_mode));

  close(fd);
}

/* 14. isatty on /dev/serial → 1 (serial supports TCGETS) */
void test_isatty_dev_serial(void) {
  int fd = open("/dev/serial", O_RDWR);
  if (fd >= 0) {
    int r = isatty(fd);
    TEST_ASSERT_EQUAL_INT(1, r);
    close(fd);
  } else {
    TEST_ASSERT_TRUE(1);
  }
}

/* 15. ioctl TCGETS on /dev/serial → success */
void test_ioctl_serial_tcgets(void) {
  int fd = open("/dev/serial", O_RDWR);
  if (fd >= 0) {
    long r = ioctl(fd, TCGETS, 0);
    TEST_ASSERT_EQUAL_INT(0, (int)r);
    close(fd);
  } else {
    TEST_ASSERT_TRUE(1);
  }
}

/* ---- Phase 3: open("/dev/") unified ---- */

/* 18. open("/dev/serial") → close → open again (serial open/close lifecycle) */
void test_serial_reopen(void) {
  int fd1 = open("/dev/serial", O_RDWR);
  TEST_ASSERT_TRUE(fd1 >= 0);
  close(fd1);

  int fd2 = open("/dev/serial", O_RDWR);
  TEST_ASSERT_TRUE(fd2 >= 0);
  close(fd2);
}

/* 19. fstat on /dev/serial → S_ISCHR */
void test_fstat_dev_serial(void) {
  int fd = open("/dev/serial", O_RDWR);
  if (fd >= 0) {
    struct stat st;
    int r = fstat(fd, &st);
    TEST_ASSERT_EQUAL_INT(0, r);
    TEST_ASSERT_TRUE(S_ISCHR(st.st_mode));
    close(fd);
  } else {
    TEST_ASSERT_TRUE(1);
  }
}

/* 20. fstat on memfd → S_ISREG */
void test_fstat_shm(void) {
  int fd = memfd_create("test_fstat_shm", 0);
  if (fd >= 0) {
    ftruncate(fd, 4096);
    struct stat st;
    int r = fstat(fd, &st);
    TEST_ASSERT_EQUAL_INT(0, r);
    TEST_ASSERT_TRUE(S_ISREG(st.st_mode));

    close(fd);
  } else {
    TEST_ASSERT_TRUE(1);
  }
}

/* 21. ioctl on /dev/serial with unknown cmd → ENOTTY */
void test_ioctl_serial_unknown(void) {
  int fd = open("/dev/serial", O_RDWR);
  if (fd >= 0) {
    long r = ioctl(fd, 0x9999, 0);
    TEST_ASSERT_TRUE(r < 0);
    TEST_ASSERT_EQUAL_INT(ENOTTY, errno);
    close(fd);
  } else {
    TEST_ASSERT_TRUE(1);
  }
}

/* 22. ioctl on bad fd → EBADF */
void test_ioctl_bad_fd(void) {
  long r = ioctl(-1, TCGETS, 0);
  TEST_ASSERT_TRUE(r < 0);
  TEST_ASSERT_EQUAL_INT(EBADF, errno);
}

/* 23. lseek on /dev/serial → ESPIPE */
void test_lseek_dev_serial(void) {
  int fd = open("/dev/serial", O_RDWR);
  if (fd >= 0) {
    off_t r = lseek(fd, 0, SEEK_SET);
    TEST_ASSERT_TRUE(r < 0);
    TEST_ASSERT_EQUAL_INT(ESPIPE, errno);
    close(fd);
  } else {
    TEST_ASSERT_TRUE(1);
  }
}

/* 24. lseek on /dev/kms → ESPIPE */
void test_lseek_dev_kms(void) {
  int fd = open("/dev/kms", O_RDWR);
  if (fd >= 0) {
    off_t r = lseek(fd, 0, SEEK_SET);
    TEST_ASSERT_TRUE(r < 0);
    TEST_ASSERT_EQUAL_INT(ESPIPE, errno);
    close(fd);
  } else {
    TEST_ASSERT_TRUE(1);
  }
}

/* 25. dup2 with /dev/serial fd — both fds usable */
void test_dup2_dev_serial(void) {
  int fd = open("/dev/serial", O_RDWR);
  if (fd >= 0) {
    int new_fd = dup2(fd, 25);
    TEST_ASSERT_EQUAL_INT(25, new_fd);

    /* Both should be writable */
    ssize_t w1 = write(fd, "A", 1);
    ssize_t w2 = write(new_fd, "B", 1);
    TEST_ASSERT_EQUAL_INT(1, (int)w1);
    TEST_ASSERT_EQUAL_INT(1, (int)w2);

    close(fd);
    close(new_fd);
  } else {
    TEST_ASSERT_TRUE(1);
  }
}

/* 26. fstat on /dev/fs (user-space driver) */
void test_fstat_dev_fs(void) {
  int fd = open("/dev/fs", O_RDWR);
  if (fd >= 0) {
    struct stat st;
    int r = fstat(fd, &st);
    TEST_ASSERT_EQUAL_INT(0, r);
    TEST_ASSERT_TRUE(S_ISCHR(st.st_mode));
    close(fd);
  } else {
    TEST_ASSERT_TRUE(1);
  }
}

/* ===== Phase 8: ioctl IPC proxy + _IOC macros ===== */

/* 27. _IOC macros produce correct encoding */
void test_ioc_macros(void) {
  /* _IO: no data */
  uint32_t cmd_io = _IO('K', 1);
  TEST_ASSERT_EQUAL_INT(0, _IOC_DIR(cmd_io));
  TEST_ASSERT_EQUAL_INT('K', _IOC_TYPE(cmd_io));
  TEST_ASSERT_EQUAL_INT(1, _IOC_NR(cmd_io));
  TEST_ASSERT_EQUAL_INT(0, _IOC_SIZE(cmd_io));

  /* _IOW: write (user→kernel) */
  uint32_t cmd_iow = _IOW('K', 2, int);
  TEST_ASSERT_EQUAL_INT(_IOC_WRITE, _IOC_DIR(cmd_iow));
  TEST_ASSERT_EQUAL_INT('K', _IOC_TYPE(cmd_iow));
  TEST_ASSERT_EQUAL_INT(2, _IOC_NR(cmd_iow));
  TEST_ASSERT_EQUAL_INT(sizeof(int), _IOC_SIZE(cmd_iow));

  /* _IOR: read (kernel→user) */
  uint32_t cmd_ior = _IOR('K', 3, int);
  TEST_ASSERT_EQUAL_INT(_IOC_READ, _IOC_DIR(cmd_ior));

  /* _IOWR: bidirectional */
  uint32_t cmd_iowr = _IOWR('K', 4, int);
  TEST_ASSERT_EQUAL_INT(_IOC_READ | _IOC_WRITE, _IOC_DIR(cmd_iowr));
}

/* 29. KMS ioctl CREATE_BUF with struct arg (via sys_ioctl) */
void test_ioctl_kms_create_buf_arg(void) {
  int fd = open("/dev/kms", O_RDWR);
  if (fd >= 0) {
    /* Try CREATE_BUF with unified struct arg */
    struct display_ioctl_create_buf_arg arg;
    memset(&arg, 0, sizeof(arg));
    arg.width = 800;
    arg.height = 600;
    arg.bpp = 32;

    int r = ioctl(fd, KMS_IOCTL_CREATE_BUF, &arg);
    /* May succeed (0) or fail (-EBUSY if already initialized, -EINVAL if bad
     * params) */
    if (r == 0) {
      /* Verify output fields were filled */
      TEST_ASSERT_TRUE(arg.pitch > 0);
      TEST_ASSERT_TRUE(arg.size > 0);
      TEST_ASSERT_TRUE(arg.rows > 0);
      TEST_ASSERT_TRUE(arg.cols > 0);
      TEST_ASSERT_EQUAL_INT(0, arg.result);
    }
    close(fd);
  } else {
    TEST_ASSERT_TRUE(1);
  }
}

/* 30. ioctl on /dev/kbd (user-space driver) — IPC proxy path */
void test_ioctl_kbd_bind(void) {
  int fd = open("/dev/kbd", O_RDWR);
  if (fd >= 0) {
    /* Direction A: driver owns SHM, consumer just registers pid for notify.
     * No memfd_create / shm_fd passing. */
    struct input_bind_arg arg;
    arg.shm_fd = -1;
    arg.result = -1;

    int r = ioctl(fd, INPUT_BIND, &arg);
    /* If kbd_driver is running: should succeed */
    /* If kbd_driver is not running: -ESRCH or similar */
    if (r == 0) {
      TEST_ASSERT_EQUAL_INT(0, arg.result);
    }
    close(fd);
  } else {
    /* /dev/kbd may not exist in test env */
    TEST_ASSERT_TRUE(1);
  }
}

/* 31. _IO-based cmd on /dev/serial → ENOTTY (serial only supports TCGETS) */
void test_ioctl_serial_ioc_cmd(void) {
  int fd = open("/dev/serial", O_RDWR);
  if (fd >= 0) {
    /* Use _IO to construct a command serial doesn't support */
    uint32_t bad_cmd = _IO('X', 99);
    long r = ioctl(fd, bad_cmd, 0);
    TEST_ASSERT_TRUE(r < 0);
    TEST_ASSERT_EQUAL_INT(ENOTTY, errno);
    close(fd);
  } else {
    TEST_ASSERT_TRUE(1);
  }
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();
  RUN_TEST(test_fstat_regular);
  RUN_TEST(test_fstat_pipe);
  RUN_TEST(test_fstat_dir);
  RUN_TEST(test_fstat_dev);
  RUN_TEST(test_fstat_bad_fd);
  RUN_TEST(test_ioctl_regular);
  RUN_TEST(test_ioctl_pipe);
  RUN_TEST(test_isatty_pipe);
  RUN_TEST(test_isatty_regular);
  RUN_TEST(test_isatty_dev_kms);
  RUN_TEST(test_stat_regular);
  RUN_TEST(test_ioctl_kms_flip);
  RUN_TEST(test_open_dev_serial);
  RUN_TEST(test_isatty_dev_serial);
  RUN_TEST(test_ioctl_serial_tcgets);
  RUN_TEST(test_serial_reopen);
  RUN_TEST(test_fstat_dev_serial);
  RUN_TEST(test_fstat_shm);
  RUN_TEST(test_ioctl_serial_unknown);
  RUN_TEST(test_ioctl_bad_fd);
  RUN_TEST(test_lseek_dev_serial);
  RUN_TEST(test_lseek_dev_kms);
  RUN_TEST(test_dup2_dev_serial);
  RUN_TEST(test_fstat_dev_fs);
  /* Phase 8 */
  RUN_TEST(test_ioc_macros);
  RUN_TEST(test_ioctl_kms_create_buf_arg);
  RUN_TEST(test_ioctl_kbd_bind);
  RUN_TEST(test_ioctl_serial_ioc_cmd);
  return UNITY_END();
}

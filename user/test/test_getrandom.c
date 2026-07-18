/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_getrandom — 随机熵子系统 Unity tests (random.md §5.3). */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/process.h>
#include <sys/random.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unity.h>
#include <xos/errno.h>

void setUp(void) {}
void tearDown(void) {}

/* 1. 两次 getrandom(32B) 结果不同 */
void test_basic_unique(void) {
  uint8_t a[32], b[32];
  TEST_ASSERT_EQUAL_INT(32, (int)getrandom(a, sizeof(a), 0));
  TEST_ASSERT_EQUAL_INT(32, (int)getrandom(b, sizeof(b), 0));
  TEST_ASSERT_TRUE(memcmp(a, b, sizeof(a)) != 0);
}

/* 2. len=0 返回 0 */
void test_len_zero(void) {
  uint8_t buf[8];
  TEST_ASSERT_EQUAL_INT(0, (int)getrandom(buf, 0, 0));
}

/* 3. 边界长度均全量返回 */
void test_len_boundary(void) {
  static uint8_t buf[4096];
  const size_t lens[] = {1, 255, 256, 257, 4096};
  for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++)
    TEST_ASSERT_EQUAL_INT((int)lens[i], (int)getrandom(buf, lens[i], 0));
}

/* 4. 1 MiB 全量返回，非全零，前后两段不同 */
void test_large(void) {
  enum { LEN = 1 << 20 };
  uint8_t *buf = malloc(LEN);
  TEST_ASSERT_NOT_NULL(buf);
  memset(buf, 0, LEN);
  TEST_ASSERT_EQUAL_INT(LEN, (int)getrandom(buf, LEN, 0));
  int all_zero = 1;
  for (int i = 0; i < LEN; i++) {
    if (buf[i] != 0) {
      all_zero = 0;
      break;
    }
  }
  TEST_ASSERT_FALSE(all_zero);
  TEST_ASSERT_TRUE(memcmp(buf, buf + LEN / 2, 4096) != 0);
  free(buf);
}

/* 5. 非法 flags → EINVAL */
void test_flags_invalid(void) {
  uint8_t buf[8];
  TEST_ASSERT_EQUAL_INT(-1, (int)getrandom(buf, sizeof(buf), 0x8));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
}

/* 6. 合法 flags（含组合）均成功 */
void test_flags_valid(void) {
  uint8_t buf[16];
  const unsigned flags[] = {GRND_NONBLOCK, GRND_RANDOM, GRND_INSECURE,
                            GRND_NONBLOCK | GRND_RANDOM | GRND_INSECURE};
  for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++)
    TEST_ASSERT_EQUAL_INT(16, (int)getrandom(buf, sizeof(buf), flags[i]));
}

/* 7. 非法 buf → EFAULT */
void test_bad_buf(void) {
  TEST_ASSERT_EQUAL_INT(-1, (int)getrandom((void *)1, 32, 0));
  TEST_ASSERT_EQUAL_INT(EFAULT, errno);
}

/* 8. getentropy 上限：>256 → EIO，256 → 0 */
void test_getentropy_limit(void) {
  static uint8_t buf[257];
  TEST_ASSERT_EQUAL_INT(-1, getentropy(buf, 257));
  TEST_ASSERT_EQUAL_INT(EIO, errno);
  TEST_ASSERT_EQUAL_INT(0, getentropy(buf, 256));
}

/* 9. arc4random_buf 两次不同；arc4random_uniform(6) 无越界且六值均出现 */
void test_arc4random(void) {
  uint8_t a[32], b[32];
  arc4random_buf(a, sizeof(a));
  arc4random_buf(b, sizeof(b));
  TEST_ASSERT_TRUE(memcmp(a, b, sizeof(a)) != 0);

  int seen[6] = {0};
  for (int i = 0; i < 10000; i++) {
    uint32_t v = arc4random_uniform(6);
    TEST_ASSERT_TRUE(v < 6);
    seen[v]++;
  }
  for (int i = 0; i < 6; i++)
    TEST_ASSERT_TRUE(seen[i] > 0);
}

/* 10. /dev/urandom 读 32B 成功，两次不同 */
void test_dev_urandom(void) {
  int fd = open("/dev/urandom", O_RDONLY);
  TEST_ASSERT_TRUE(fd >= 0);
  uint8_t a[32], b[32];
  TEST_ASSERT_EQUAL_INT(32, (int)read(fd, a, sizeof(a)));
  TEST_ASSERT_EQUAL_INT(32, (int)read(fd, b, sizeof(b)));
  TEST_ASSERT_TRUE(memcmp(a, b, sizeof(a)) != 0);
  close(fd);
}

/* 11. /dev/random 行为与 urandom 一致 */
void test_dev_random(void) {
  int fd = open("/dev/random", O_RDONLY);
  TEST_ASSERT_TRUE(fd >= 0);
  uint8_t a[32], b[32];
  TEST_ASSERT_EQUAL_INT(32, (int)read(fd, a, sizeof(a)));
  TEST_ASSERT_EQUAL_INT(32, (int)read(fd, b, sizeof(b)));
  TEST_ASSERT_TRUE(memcmp(a, b, sizeof(a)) != 0);
  close(fd);
}

/* 12. fork 后父子各取 32B 不同（pipe 回传比对） */
void test_fork_diverge(void) {
  int pfd[2];
  TEST_ASSERT_EQUAL_INT(0, pipe(pfd));
  pid_t pid = fork();
  if (pid == 0) {
    uint8_t cbuf[32];
    if (getrandom(cbuf, sizeof(cbuf), 0) != 32)
      _exit(1);
    if (write(pfd[1], cbuf, sizeof(cbuf)) != 32)
      _exit(1);
    _exit(0);
  }
  uint8_t pbuf[32], cbuf[32];
  TEST_ASSERT_EQUAL_INT(32, (int)getrandom(pbuf, sizeof(pbuf), 0));
  TEST_ASSERT_EQUAL_INT(32, (int)read(pfd[0], cbuf, sizeof(cbuf)));
  int status;
  TEST_ASSERT_TRUE(waitpid(pid, &status, 0) == pid);
  TEST_ASSERT_EQUAL_INT(0, status);
  TEST_ASSERT_TRUE(memcmp(pbuf, cbuf, sizeof(pbuf)) != 0);
  close(pfd[0]);
  close(pfd[1]);
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();
  RUN_TEST(test_basic_unique);
  RUN_TEST(test_len_zero);
  RUN_TEST(test_len_boundary);
  RUN_TEST(test_large);
  RUN_TEST(test_flags_invalid);
  RUN_TEST(test_flags_valid);
  RUN_TEST(test_bad_buf);
  RUN_TEST(test_getentropy_limit);
  RUN_TEST(test_arc4random);
  RUN_TEST(test_dev_urandom);
  RUN_TEST(test_dev_random);
  RUN_TEST(test_fork_diverge);
  return UNITY_END();
}

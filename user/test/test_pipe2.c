/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_pipe2.c — pipe2(fd, flags) syscall tests.
 *
 * pipe2 extends pipe with a flags argument (Linux semantics):
 *   - flags == 0 behaves exactly like pipe().
 *   - O_CLOEXEC marks both fds close-on-exec (per-fd bitmap, F_GETFD).
 *   - O_NONBLOCK sets nonblocking mode on both ends (F_GETFL), so a read on
 *     an empty pipe returns -1/EAGAIN instead of blocking.
 *   - Any other flag is rejected with EINVAL.
 */
#include "unity.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>

void setUp(void) {}
void tearDown(void) {}

/* pipe2(fd, 0) is equivalent to pipe(): plain blocking pipe, no cloexec,
 * data roundtrips. */
void test_pipe2_zero_equals_pipe(void) {
  int fd[2];
  TEST_ASSERT_EQUAL_INT(0, pipe2(fd, 0));

  TEST_ASSERT_EQUAL_INT(0, fcntl(fd[0], F_GETFD));
  TEST_ASSERT_EQUAL_INT(0, fcntl(fd[1], F_GETFD));
  TEST_ASSERT_EQUAL_INT(0, fcntl(fd[0], F_GETFL) & O_NONBLOCK);
  TEST_ASSERT_EQUAL_INT(0, fcntl(fd[1], F_GETFL) & O_NONBLOCK);

  const char msg[] = "pipe2";
  TEST_ASSERT_EQUAL_INT((int)sizeof(msg), (int)write(fd[1], msg, sizeof(msg)));
  char buf[sizeof(msg)] = {0};
  TEST_ASSERT_EQUAL_INT((int)sizeof(msg), (int)read(fd[0], buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING(msg, buf);

  close(fd[0]);
  close(fd[1]);
}

/* O_CLOEXEC sets FD_CLOEXEC on both fds. */
void test_pipe2_cloexec(void) {
  int fd[2];
  TEST_ASSERT_EQUAL_INT(0, pipe2(fd, O_CLOEXEC));

  TEST_ASSERT_EQUAL_INT(FD_CLOEXEC, fcntl(fd[0], F_GETFD));
  TEST_ASSERT_EQUAL_INT(FD_CLOEXEC, fcntl(fd[1], F_GETFD));

  close(fd[0]);
  close(fd[1]);
}

/* O_NONBLOCK lands in f->flags on both ends (F_GETFL) and makes an empty
 * read fail with EAGAIN instead of blocking. */
void test_pipe2_nonblock(void) {
  int fd[2];
  TEST_ASSERT_EQUAL_INT(0, pipe2(fd, O_NONBLOCK));

  TEST_ASSERT_EQUAL_INT(O_NONBLOCK, fcntl(fd[0], F_GETFL) & O_NONBLOCK);
  TEST_ASSERT_EQUAL_INT(O_NONBLOCK, fcntl(fd[1], F_GETFL) & O_NONBLOCK);

  char c;
  errno = 0;
  TEST_ASSERT_EQUAL_INT(-1, (int)read(fd[0], &c, 1));
  TEST_ASSERT_EQUAL_INT(EAGAIN, errno);

  close(fd[0]);
  close(fd[1]);
}

/* Flags other than O_CLOEXEC|O_NONBLOCK are rejected with EINVAL. */
void test_pipe2_invalid_flags(void) {
  int fd[2];
  errno = 0;
  TEST_ASSERT_EQUAL_INT(-1, pipe2(fd, O_APPEND));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_pipe2_zero_equals_pipe);
  RUN_TEST(test_pipe2_cloexec);
  RUN_TEST(test_pipe2_nonblock);
  RUN_TEST(test_pipe2_invalid_flags);
  return UNITY_END();
}

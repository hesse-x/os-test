/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_eventfd — eventfd Unity tests (design §8.3, EF-001~010). */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/poll.h>
#include <sys/process.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unity.h>
#include <xos/errno.h>

void setUp(void) {}
void tearDown(void) {}

/* EF-001 */
void test_eventfd_create(void) {
  int fd = eventfd(0, 0);
  TEST_ASSERT_TRUE(fd >= 0);
  close(fd);
}

/* EF-002 */
void test_eventfd_cloexec(void) {
  int fd = eventfd(0, EFD_CLOEXEC);
  TEST_ASSERT_TRUE(fd >= 0);
  int flags = fcntl(fd, F_GETFD);
  TEST_ASSERT_TRUE(flags & FD_CLOEXEC);
  close(fd);
}

/* EF-003 */
void test_eventfd_write_read(void) {
  int fd = eventfd(0, 0);
  uint64_t val = 42;
  TEST_ASSERT_EQUAL_INT(8, (int)write(fd, &val, 8));
  uint64_t out = 0;
  TEST_ASSERT_EQUAL_INT(8, (int)read(fd, &out, 8));
  TEST_ASSERT_EQUAL_UINT64(42, out);
  close(fd);
}

/* EF-004 */
void test_eventfd_initval(void) {
  int fd = eventfd(5, 0);
  uint64_t out = 0;
  TEST_ASSERT_EQUAL_INT(8, (int)read(fd, &out, 8));
  TEST_ASSERT_EQUAL_UINT64(5, out);
  close(fd);
}

/* EF-005 */
void test_eventfd_read_clears(void) {
  int fd = eventfd(0, 0);
  uint64_t val = 7;
  write(fd, &val, 8);
  uint64_t out = 0;
  read(fd, &out, 8);
  TEST_ASSERT_EQUAL_UINT64(7, out);
  /* Second read blocks; use nonblock to get EAGAIN. */
  int fd2 = eventfd(0, EFD_NONBLOCK);
  write(fd2, &val, 8);
  read(fd2, &out, 8);
  TEST_ASSERT_EQUAL_INT(-1, (int)read(fd2, &out, 8));
  TEST_ASSERT_EQUAL_INT(EAGAIN, errno);
  close(fd);
  close(fd2);
}

/* EF-006 */
void test_eventfd_nonblock_eagain(void) {
  int fd = eventfd(0, EFD_NONBLOCK);
  uint64_t out = 0;
  TEST_ASSERT_EQUAL_INT(-1, (int)read(fd, &out, 8));
  TEST_ASSERT_EQUAL_INT(EAGAIN, errno);
  close(fd);
}

/* EF-007 */
void test_eventfd_semaphore(void) {
  int fd = eventfd(0, EFD_SEMAPHORE | EFD_NONBLOCK);
  uint64_t val = 3;
  write(fd, &val, 8);
  uint64_t out;
  for (int i = 0; i < 3; i++) {
    TEST_ASSERT_EQUAL_INT(8, (int)read(fd, &out, 8));
    TEST_ASSERT_EQUAL_UINT64(1, out);
  }
  TEST_ASSERT_EQUAL_INT(-1, (int)read(fd, &out, 8));
  close(fd);
}

/* EF-008 */
void test_eventfd_overflow(void) {
  int fd = eventfd(0, EFD_NONBLOCK);
  /* Fill up to EVENTFD_MAX. */
  uint64_t big = 0xFFFFFFFFFFFFFFFEULL;
  TEST_ASSERT_EQUAL_INT(8, (int)write(fd, &big, 8));
  /* Next write would overflow → EAGAIN (nonblock). */
  uint64_t one = 1;
  TEST_ASSERT_EQUAL_INT(-1, (int)write(fd, &one, 8));
  TEST_ASSERT_EQUAL_INT(EAGAIN, errno);
  close(fd);
}

/* EF-009 */
void test_eventfd_poll(void) {
  int fd = eventfd(0, 0);
  /* Empty → no POLLIN. */
  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  TEST_ASSERT_EQUAL_INT(0, poll(&pfd, 1, 0));
  uint64_t val = 1;
  write(fd, &val, 8);
  pfd.revents = 0;
  TEST_ASSERT_TRUE(poll(&pfd, 1, 100) > 0);
  TEST_ASSERT_TRUE(pfd.revents & POLLIN);
  close(fd);
}

/* EF-010 */
void test_eventfd_blocking_wake(void) {
  int fd = eventfd(0, 0);
  pid_t pid = fork();
  if (pid == 0) {
    usleep(50 * 1000);
    uint64_t val = 1;
    write(fd, &val, 8);
    _exit(0);
  }
  uint64_t out = 0;
  int r = read(fd, &out, 8);
  TEST_ASSERT_EQUAL_INT(8, r);
  TEST_ASSERT_EQUAL_UINT64(1, out);
  int status;
  waitpid(pid, &status, 0);
  close(fd);
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();
  RUN_TEST(test_eventfd_create);
  RUN_TEST(test_eventfd_cloexec);
  RUN_TEST(test_eventfd_write_read);
  RUN_TEST(test_eventfd_initval);
  RUN_TEST(test_eventfd_read_clears);
  RUN_TEST(test_eventfd_nonblock_eagain);
  RUN_TEST(test_eventfd_semaphore);
  RUN_TEST(test_eventfd_overflow);
  RUN_TEST(test_eventfd_poll);
  RUN_TEST(test_eventfd_blocking_wake);
  return UNITY_END();
}

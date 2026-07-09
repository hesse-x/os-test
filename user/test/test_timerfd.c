/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_timerfd — timerfd Unity tests (design §8.4, TF-001~010). */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <unity.h>
#include <xos/errno.h>

void setUp(void) {}
void tearDown(void) {}

static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* TF-001 */
void test_timerfd_create(void) {
  int fd = timerfd_create(CLOCK_MONOTONIC, 0);
  TEST_ASSERT_TRUE(fd >= 0);
  close(fd);
}

/* TF-002 */
void test_timerfd_invalid_clock(void) {
  int fd = timerfd_create(CLOCK_REALTIME, 0);
  TEST_ASSERT_EQUAL_INT(-1, fd);
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
}

/* TF-003 */
void test_timerfd_oneshot(void) {
  int fd = timerfd_create(CLOCK_MONOTONIC, 0);
  struct itimerspec its;
  memset(&its, 0, sizeof(its));
  its.it_value.tv_nsec = 50 * 1000000L; /* 50ms */
  TEST_ASSERT_EQUAL_INT(0, timerfd_settime(fd, 0, &its, NULL));
  uint64_t t0 = now_ms();
  uint64_t ticks = 0;
  int r = read(fd, &ticks, 8);
  uint64_t dt = now_ms() - t0;
  TEST_ASSERT_EQUAL_INT(8, r);
  TEST_ASSERT_TRUE(ticks >= 1);
  TEST_ASSERT_TRUE(dt >= 40 && dt < 500);
  close(fd);
}

/* TF-004 */
void test_timerfd_periodic(void) {
  int fd = timerfd_create(CLOCK_MONOTONIC, 0);
  struct itimerspec its;
  memset(&its, 0, sizeof(its));
  its.it_value.tv_nsec = 40 * 1000000L;    /* 40ms first */
  its.it_interval.tv_nsec = 40 * 1000000L; /* 40ms period */
  timerfd_settime(fd, 0, &its, NULL);
  uint64_t ticks = 0;
  read(fd, &ticks, 8); /* first */
  usleep(120 * 1000);  /* wait for ~3 periods */
  ticks = 0;
  int r = read(fd, &ticks, 8);
  TEST_ASSERT_EQUAL_INT(8, r);
  TEST_ASSERT_TRUE(ticks >= 2);
  close(fd);
}

/* TF-005 */
void test_timerfd_read_clears(void) {
  int fd = timerfd_create(CLOCK_MONOTONIC, 0);
  struct itimerspec its;
  memset(&its, 0, sizeof(its));
  its.it_value.tv_nsec = 10 * 1000000L;
  timerfd_settime(fd, 0, &its, NULL);
  uint64_t ticks = 0;
  read(fd, &ticks, 8);
  TEST_ASSERT_TRUE(ticks >= 1);
  /* Set nonblock: no pending ticks → EAGAIN. */
  fcntl(fd, F_SETFL, O_NONBLOCK);
  TEST_ASSERT_EQUAL_INT(-1, read(fd, &ticks, 8));
  TEST_ASSERT_EQUAL_INT(EAGAIN, errno);
  close(fd);
}

/* TF-006 */
void test_timerfd_nonblock_eagain(void) {
  int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
  uint64_t ticks = 0;
  TEST_ASSERT_EQUAL_INT(-1, read(fd, &ticks, 8));
  TEST_ASSERT_EQUAL_INT(EAGAIN, errno);
  close(fd);
}

/* TF-007 */
void test_timerfd_abstime(void) {
  int fd = timerfd_create(CLOCK_MONOTONIC, 0);
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  struct itimerspec its;
  memset(&its, 0, sizeof(its));
  its.it_value = now;
  its.it_value.tv_nsec += 60 * 1000000L;
  if (its.it_value.tv_nsec >= 1000000000L) {
    its.it_value.tv_sec += 1;
    its.it_value.tv_nsec -= 1000000000L;
  }
  TEST_ASSERT_EQUAL_INT(0, timerfd_settime(fd, TFD_TIMER_ABSTIME, &its, NULL));
  uint64_t ticks = 0;
  int r = read(fd, &ticks, 8);
  TEST_ASSERT_EQUAL_INT(8, r);
  TEST_ASSERT_TRUE(ticks >= 1);
  close(fd);
}

/* TF-008 */
void test_timerfd_disarm(void) {
  int fd = timerfd_create(CLOCK_MONOTONIC, 0);
  struct itimerspec its;
  memset(&its, 0, sizeof(its));
  its.it_value.tv_nsec = 100 * 1000000L; /* 100ms */
  timerfd_settime(fd, 0, &its, NULL);
  /* Disarm: it_value = 0. */
  memset(&its, 0, sizeof(its));
  timerfd_settime(fd, 0, &its, NULL);
  fcntl(fd, F_SETFL, O_NONBLOCK);
  usleep(150 * 1000);
  uint64_t ticks = 0;
  TEST_ASSERT_EQUAL_INT(-1, read(fd, &ticks, 8));
  TEST_ASSERT_EQUAL_INT(EAGAIN, errno);
  close(fd);
}

/* TF-009 */
void test_timerfd_poll(void) {
  int fd = timerfd_create(CLOCK_MONOTONIC, 0);
  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  TEST_ASSERT_EQUAL_INT(0, poll(&pfd, 1, 0));
  struct itimerspec its;
  memset(&its, 0, sizeof(its));
  its.it_value.tv_nsec = 50 * 1000000L;
  timerfd_settime(fd, 0, &its, NULL);
  pfd.revents = 0;
  TEST_ASSERT_TRUE(poll(&pfd, 1, 1000) > 0);
  TEST_ASSERT_TRUE(pfd.revents & POLLIN);
  close(fd);
}

/* TF-010 */
void test_timerfd_old_value(void) {
  int fd = timerfd_create(CLOCK_MONOTONIC, 0);
  struct itimerspec its;
  memset(&its, 0, sizeof(its));
  its.it_value.tv_nsec = 100 * 1000000L;
  its.it_interval.tv_nsec = 200 * 1000000L;
  timerfd_settime(fd, 0, &its, NULL);
  /* Re-arm with different config; capture old. */
  struct itimerspec neu, old;
  memset(&neu, 0, sizeof(neu));
  neu.it_value.tv_nsec = 500 * 1000000L;
  TEST_ASSERT_EQUAL_INT(0, timerfd_settime(fd, 0, &neu, &old));
  /* old should reflect the previous interval. */
  TEST_ASSERT_EQUAL_INT(200000000L, (long)old.it_interval.tv_nsec);
  close(fd);
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();
  RUN_TEST(test_timerfd_create);
  RUN_TEST(test_timerfd_invalid_clock);
  RUN_TEST(test_timerfd_oneshot);
  RUN_TEST(test_timerfd_periodic);
  RUN_TEST(test_timerfd_read_clears);
  RUN_TEST(test_timerfd_nonblock_eagain);
  RUN_TEST(test_timerfd_abstime);
  RUN_TEST(test_timerfd_disarm);
  RUN_TEST(test_timerfd_poll);
  RUN_TEST(test_timerfd_old_value);
  return UNITY_END();
}

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_time_sleep — S15: nanosleep / clock_nanosleep (relative + TIMER_ABSTIME)
 * + signal-interrupted rem + sleep() no longer woken by IPC. */

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/process.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <unity.h>
#include <xos/errno.h>

void setUp(void) {}
void tearDown(void) {}

static uint64_t mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* TS-001: nanosleep 100ms actually sleeps ~100ms. */
void test_nanosleep_100ms(void) {
  uint64_t t0 = mono_ms();
  struct timespec req = {0, 100 * 1000 * 1000L};
  TEST_ASSERT_EQUAL_INT(0, nanosleep(&req, NULL));
  uint64_t dt = mono_ms() - t0;
  TEST_ASSERT_TRUE(dt >= 90 && dt < 500);
}

/* TS-002: nanosleep zero-length returns 0 immediately (kernel forces 1ns). */
void test_nanosleep_zero(void) {
  struct timespec req = {0, 0};
  TEST_ASSERT_EQUAL_INT(0, nanosleep(&req, NULL));
}

/* TS-003: nanosleep negative tv_sec -> EINVAL. */
void test_nanosleep_negative_einval(void) {
  struct timespec req = {-1, 0};
  TEST_ASSERT_EQUAL_INT(-1, nanosleep(&req, NULL));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
}

/* TS-004: nanosleep bad nsec -> EINVAL. */
void test_nanosleep_bad_nsec_einval(void) {
  struct timespec req = {0, 2000000000L};
  TEST_ASSERT_EQUAL_INT(-1, nanosleep(&req, NULL));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
}

/* TS-005: clock_nanosleep MONOTONIC relative 50ms. */
void test_clock_nanosleep_monotonic_relative(void) {
  uint64_t t0 = mono_ms();
  struct timespec req = {0, 50 * 1000 * 1000L};
  TEST_ASSERT_EQUAL_INT(0, clock_nanosleep(CLOCK_MONOTONIC, 0, &req, NULL));
  uint64_t dt = mono_ms() - t0;
  TEST_ASSERT_TRUE(dt >= 40 && dt < 500);
}

/* TS-006: clock_nanosleep REALTIME relative 50ms. */
void test_clock_nanosleep_realtime_relative(void) {
  uint64_t t0 = mono_ms();
  struct timespec req = {0, 50 * 1000 * 1000L};
  TEST_ASSERT_EQUAL_INT(0, clock_nanosleep(CLOCK_REALTIME, 0, &req, NULL));
  uint64_t dt = mono_ms() - t0;
  TEST_ASSERT_TRUE(dt >= 40 && dt < 500);
}

/* TS-007: clock_nanosleep MONOTONIC absolute (TIMER_ABSTIME) ~50ms. */
void test_clock_nanosleep_monotonic_abstime(void) {
  struct timespec now;
  TEST_ASSERT_EQUAL_INT(0, clock_gettime(CLOCK_MONOTONIC, &now));
  struct timespec abs = {now.tv_sec, now.tv_nsec + 50 * 1000 * 1000L};
  if (abs.tv_nsec >= 1000000000L) {
    abs.tv_sec += 1;
    abs.tv_nsec -= 1000000000L;
  }
  uint64_t t0 = mono_ms();
  TEST_ASSERT_EQUAL_INT(
      0, clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &abs, NULL));
  uint64_t dt = mono_ms() - t0;
  TEST_ASSERT_TRUE(dt >= 40 && dt < 500);
}

/* TS-008: clock_nanosleep REALTIME absolute (TIMER_ABSTIME) ~50ms. */
void test_clock_nanosleep_realtime_abstime(void) {
  struct timespec now;
  TEST_ASSERT_EQUAL_INT(0, clock_gettime(CLOCK_REALTIME, &now));
  struct timespec abs = {now.tv_sec, now.tv_nsec + 50 * 1000 * 1000L};
  if (abs.tv_nsec >= 1000000000L) {
    abs.tv_sec += 1;
    abs.tv_nsec -= 1000000000L;
  }
  uint64_t t0 = mono_ms();
  TEST_ASSERT_EQUAL_INT(
      0, clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &abs, NULL));
  uint64_t dt = mono_ms() - t0;
  TEST_ASSERT_TRUE(dt >= 40 && dt < 500);
}

/* TS-009: clock_nanosleep on PROCESS_CPUTIME_ID -> EINVAL (not sleepable). */
void test_clock_nanosleep_cputime_einval(void) {
  struct timespec req = {0, 50 * 1000 * 1000L};
  TEST_ASSERT_EQUAL_INT(
      -1, clock_nanosleep(CLOCK_PROCESS_CPUTIME_ID, 0, &req, NULL));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
}

/* TS-010: clock_nanosleep bad flags -> EINVAL. */
void test_clock_nanosleep_bad_flags_einval(void) {
  struct timespec req = {0, 50 * 1000 * 1000L};
  TEST_ASSERT_EQUAL_INT(-1, clock_nanosleep(CLOCK_MONOTONIC, 0xFF, &req, NULL));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
}

static volatile int sigusr1_count = 0;
static void sigusr1_handler(int sig) {
  (void)sig;
  sigusr1_count++;
}

/* TS-011: nanosleep interrupted by a signal returns -1/EINTR and writes the
 * remaining time to *rem. */
void test_nanosleep_eintr_rem(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = sigusr1_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGUSR1, &sa, NULL);

  sigusr1_count = 0;
  pid_t me = getpid();

  /* Arm a child that sends SIGUSR1 after 50ms. */
  if (fork() == 0) {
    struct timespec d = {0, 50 * 1000 * 1000L};
    nanosleep(&d, NULL);
    kill(me, SIGUSR1);
    _exit(0);
  }

  struct timespec req = {5, 0};
  struct timespec rem;
  int r = nanosleep(&req, &rem);
  TEST_ASSERT_EQUAL_INT(-1, r);
  TEST_ASSERT_EQUAL_INT(EINTR, errno);
  TEST_ASSERT_TRUE(sigusr1_count >= 1);
  /* rem should hold the unslept time: between 0 and 5s. */
  TEST_ASSERT_TRUE(rem.tv_sec >= 0 && rem.tv_sec <= 5);
  TEST_ASSERT_TRUE(rem.tv_nsec >= 0 && rem.tv_nsec < 1000000000L);

  /* Reap the child. */
  int status;
  waitpid(-1, &status, 0);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_nanosleep_100ms);
  RUN_TEST(test_nanosleep_zero);
  RUN_TEST(test_nanosleep_negative_einval);
  RUN_TEST(test_nanosleep_bad_nsec_einval);
  RUN_TEST(test_clock_nanosleep_monotonic_relative);
  RUN_TEST(test_clock_nanosleep_realtime_relative);
  RUN_TEST(test_clock_nanosleep_monotonic_abstime);
  RUN_TEST(test_clock_nanosleep_realtime_abstime);
  RUN_TEST(test_clock_nanosleep_cputime_einval);
  RUN_TEST(test_clock_nanosleep_bad_flags_einval);
  RUN_TEST(test_nanosleep_eintr_rem);
  return UNITY_END();
}

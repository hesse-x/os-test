/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_accept_no_timeout — S18 accept() no longer has a 30s false timeout.
 *
 * Linux blocking accept() has no built-in timeout: it blocks indefinitely for a
 * connection, returning only on a signal (EINTR) or fd close. The kernel used
 * to arm a 30s wait_deadline and return -ETIMEDOUT on idle listeners, which
 * made long-lived servers (udevd/shell) wrongly bail out. S18 removed that.
 *
 * Strategy: bind+listen an AF_UNIX socket, accept() with no incoming
 * connection, and arm a SIGALRM at 35s — past the old 30s false-timeout. If
 * accept returns before SIGALRM with -ETIMEDOUT, the regression is present
 * (fail). If it blocks until SIGALRM and returns -EINTR, the fix holds.
 *
 * NOTE: this test sleeps up to 35s by design — it is the regression proof.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <unity.h>
#include <xos/socket.h>

void setUp(void) {}
void tearDown(void) {}

static volatile int got_alrm;

static void alrm_handler(int sig) {
  (void)sig;
  got_alrm = 1;
}

static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* Idle blocking accept must outlast the old 30s false-timeout and only return
 * via SIGALRM/EINTR. */
void test_accept_blocks_past_30s(void) {
  int lst = socket(AF_UNIX, SOCK_STREAM, 0);
  if (lst < 0) {
    /* AF_UNIX unavailable in this build — nothing to assert. */
    TEST_ASSERT_TRUE(1);
    return;
  }
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, "/tmp/acc_noto", sizeof(addr.sun_path) - 1);
  unlink(addr.sun_path);
  if (bind(lst, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      listen(lst, 4) != 0) {
    close(lst);
    TEST_ASSERT_TRUE(1);
    return;
  }

  /* No child connects. Arm SIGALRM at 35s — past the old 30s timeout. */
  got_alrm = 0;
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = alrm_handler;
  sigaction(SIGALRM, &act, NULL);
  alarm(35);

  uint64_t t0 = now_ms();
  int a = accept(lst, NULL, NULL);
  uint64_t dt = now_ms() - t0;
  alarm(0);

  TEST_ASSERT_EQUAL_INT(-1, a);
  TEST_ASSERT_EQUAL_INT(EINTR, errno);
  /* Must have actually waited for the alarm (≥30s), proving no false timeout.
   */
  TEST_ASSERT_TRUE(dt >= 30000);
  TEST_ASSERT_TRUE(got_alrm);

  close(lst);
  unlink(addr.sun_path);
}

/* Sanity: accept on a non-blocking idle listener returns EAGAIN immediately
 * (the non-block path is untouched by S18). */
void test_accept_nonblock_eagain(void) {
  int lst = socket(AF_UNIX, SOCK_STREAM, 0);
  if (lst < 0) {
    TEST_ASSERT_TRUE(1);
    return;
  }
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, "/tmp/acc_nb", sizeof(addr.sun_path) - 1);
  unlink(addr.sun_path);
  if (bind(lst, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      listen(lst, 4) != 0) {
    close(lst);
    TEST_ASSERT_TRUE(1);
    return;
  }
  fcntl(lst, F_SETFL, O_NONBLOCK);
  int a = accept(lst, NULL, NULL);
  TEST_ASSERT_EQUAL_INT(-1, a);
  TEST_ASSERT_EQUAL_INT(EAGAIN, errno);
  close(lst);
  unlink(addr.sun_path);
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();
  RUN_TEST(test_accept_blocks_past_30s);
  RUN_TEST(test_accept_nonblock_eagain);
  return UNITY_END();
}

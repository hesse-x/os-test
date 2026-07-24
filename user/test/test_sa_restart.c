/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_sa_restart.c — 02: SA_RESTART restarts slow syscalls interrupted by a
 * caught signal; without SA_RESTART they surface as -EINTR. Never-restart
 * syscalls (nanosleep) always return EINTR + a remaining time. */
#include "unity.h"
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/process.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

void setUp(void) {}
void tearDown(void) {}

static volatile int saw_sigusr1;
static void usr1_handler(int sig) {
  (void)sig;
  saw_sigusr1 = 1;
}

/* Child helper. MUST always _exit() — otherwise the child falls through and
 * re-runs the parent's test body, racing concurrent Unity output to the serial
 * console. signal_only=1: just interrupt the parent (and keep any write end
 * open so a blocking read sees EINTR, not EOF). signal_only=0: interrupt, then
 * after a pause write one byte so a restarted read can complete. */
static void interrupter(int write_fd, int signal_only) {
  pid_t parent = getppid();
  struct timespec ts = {0, 80 * 1000 * 1000}; /* 80ms */
  nanosleep(&ts, NULL);
  kill(parent, SIGUSR1);
  if (signal_only) {
    /* Linger holding any write end open; the parent reaps/kills us. */
    struct timespec d = {3, 0};
    nanosleep(&d, NULL);
    _exit(0);
  }
  ts.tv_sec = 0;
  ts.tv_nsec = 150 * 1000 * 1000; /* 150ms */
  nanosleep(&ts, NULL);
  if (write_fd >= 0)
    write(write_fd, "X", 1);
  _exit(0);
}

static void reap_child(pid_t child) {
  int status = 0;
  /* Kill first so a lingering signal_only child stops sleeping, then reap. */
  kill(child, SIGTERM);
  while (waitpid(child, &status, 0) < 0 && errno == EINTR)
    ;
}

/* read(pipe) with SA_RESTART: the signal interrupts, the handler runs, and the
 * read is re-executed — it stays blocked until data arrives, then returns 1.
 * Pre-fix: read returned -1/EINTR. */
void test_read_restart_with_sa_restart(void) {
  saw_sigusr1 = 0;
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = usr1_handler;
  act.sa_flags = SA_RESTART;
  TEST_ASSERT_EQUAL_INT(0, sigaction(SIGUSR1, &act, NULL));

  int p[2];
  TEST_ASSERT_EQUAL_INT(0, pipe(p));

  pid_t child = fork();
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, child);
  if (child == 0) {
    close(p[0]);
    interrupter(p[1], 0);
  }
  close(p[1]);

  char buf[4] = {0};
  int r = read(p[0], buf, sizeof(buf));
  TEST_ASSERT_EQUAL_INT(1, r);
  TEST_ASSERT_EQUAL_INT('X', buf[0]);
  TEST_ASSERT_EQUAL_INT(1, saw_sigusr1);

  close(p[0]);
  reap_child(child);

  memset(&act, 0, sizeof(act));
  act.sa_handler = SIG_DFL;
  sigaction(SIGUSR1, &act, NULL);
}

/* read(pipe) without SA_RESTART: the interrupting signal surfaces as EINTR.
 * The child keeps the write end open (no EOF) and only signals. */
void test_read_eintr_without_sa_restart(void) {
  saw_sigusr1 = 0;
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = usr1_handler;
  act.sa_flags = 0; /* no SA_RESTART */
  TEST_ASSERT_EQUAL_INT(0, sigaction(SIGUSR1, &act, NULL));

  int p[2];
  TEST_ASSERT_EQUAL_INT(0, pipe(p));

  pid_t child = fork();
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, child);
  if (child == 0) {
    close(p[0]);
    interrupter(p[1], 1);
  }
  close(p[1]);

  char buf[4];
  int r = read(p[0], buf, sizeof(buf));
  TEST_ASSERT_EQUAL_INT(-1, r);
  TEST_ASSERT_EQUAL_INT(EINTR, errno);
  TEST_ASSERT_EQUAL_INT(1, saw_sigusr1);

  close(p[0]);
  reap_child(child);

  memset(&act, 0, sizeof(act));
  act.sa_handler = SIG_DFL;
  sigaction(SIGUSR1, &act, NULL);
}

/* nanosleep is never restarted: even with SA_RESTART, an interrupt surfaces as
 * EINTR and the remaining time is written back. */
void test_nanosleep_eintr_even_with_sa_restart(void) {
  saw_sigusr1 = 0;
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = usr1_handler;
  act.sa_flags = SA_RESTART;
  TEST_ASSERT_EQUAL_INT(0, sigaction(SIGUSR1, &act, NULL));

  pid_t child = fork();
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, child);
  if (child == 0)
    interrupter(-1, 1);

  struct timespec req = {0, 500 * 1000 * 1000}; /* 500ms */
  struct timespec rem = {0, 0};
  int r = nanosleep(&req, &rem);
  TEST_ASSERT_EQUAL_INT(-1, r);
  TEST_ASSERT_EQUAL_INT(EINTR, errno);
  TEST_ASSERT_EQUAL_INT(1, saw_sigusr1);
  /* A substantial fraction of the 500ms must remain (the interrupt fires at
   * ~80ms). */
  TEST_ASSERT_GREATER_THAN_INT(0, (int)(rem.tv_nsec));

  reap_child(child);

  memset(&act, 0, sizeof(act));
  act.sa_handler = SIG_DFL;
  sigaction(SIGUSR1, &act, NULL);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_read_restart_with_sa_restart);
  RUN_TEST(test_read_eintr_without_sa_restart);
  RUN_TEST(test_nanosleep_eintr_even_with_sa_restart);
  return UNITY_END();
}

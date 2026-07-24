/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_sa_nocldwait.c — 02: SA_NOCLDWAIT makes children auto-reaped on exit —
 * they do not become zombies and do not deliver SIGCHLD. A subsequent
 * waitpid(WNOHANG) on such a child returns -1/ECHILD (no child to reap).
 * Without SA_NOCLDWAIT the child stays a zombie until waitpid reaps it. */
#include "unity.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/process.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

void setUp(void) {}
void tearDown(void) {}

static volatile int sigchld_count;
static void sigchld_handler(int sig) {
  (void)sig;
  sigchld_count++;
}

/* SA_NOCLDWAIT: child auto-reaped. waitpid(WNOHANG) → ECHILD; SIGCHLD not
 * delivered. */
void test_sa_nocldwait_auto_reaps(void) {
  sigchld_count = 0;
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = sigchld_handler;
  act.sa_flags = SA_NOCLDWAIT | SA_RESTART;
  TEST_ASSERT_EQUAL_INT(0, sigaction(SIGCHLD, &act, NULL));

  pid_t child = fork();
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, child);
  if (child == 0)
    _exit(0);

  /* Give the child a moment to exit and be auto-reaped. */
  struct timespec ts = {0, 50 * 1000 * 1000}; /* 50ms */
  nanosleep(&ts, NULL);

  int status = 0;
  pid_t r = waitpid(child, &status, WNOHANG);
  TEST_ASSERT_EQUAL_INT(-1, r);
  TEST_ASSERT_EQUAL_INT(ECHILD, errno);

  /* Auto-reap skips SIGCHLD posting entirely. */
  TEST_ASSERT_EQUAL_INT(0, sigchld_count);

  /* Restore default. */
  memset(&act, 0, sizeof(act));
  act.sa_handler = SIG_DFL;
  sigaction(SIGCHLD, &act, NULL);
}

/* Without SA_NOCLDWAIT: the child stays a zombie until an explicit waitpid
 * collects it (default disposition does not auto-reap). WNOHANG on a zombie
 * reaps it and returns the pid — contrast SA_NOCLDWAIT above, which auto-reaps
 * and yields ECHILD. */
void test_default_leaves_zombie(void) {
  sigchld_count = 0;
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = sigchld_handler;
  act.sa_flags = SA_RESTART; /* explicitly NOT SA_NOCLDWAIT */
  TEST_ASSERT_EQUAL_INT(0, sigaction(SIGCHLD, &act, NULL));

  pid_t child = fork();
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, child);
  if (child == 0)
    _exit(0);

  /* Wait for the SIGCHLD-driven wake (the handler increments sigchld_count). */
  int spins = 0;
  while (sigchld_count == 0 && spins < 200) {
    struct timespec ts = {0, 5 * 1000 * 1000}; /* 5ms */
    nanosleep(&ts, NULL);
    spins++;
  }
  TEST_ASSERT_GREATER_THAN_INT(0, sigchld_count);

  /* Child is a zombie now: WNOHANG reaps it and returns its pid. */
  int status = 0;
  pid_t r = waitpid(child, &status, WNOHANG);
  TEST_ASSERT_EQUAL_INT(child, r);
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));

  memset(&act, 0, sizeof(act));
  act.sa_handler = SIG_DFL;
  sigaction(SIGCHLD, &act, NULL);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_sa_nocldwait_auto_reaps);
  RUN_TEST(test_default_leaves_zombie);
  return UNITY_END();
}

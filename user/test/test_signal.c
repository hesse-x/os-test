/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "user/test/test_helpers.h"
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

/* 1. kill invalid pid returns -ESRCH */
void test_kill_invalid_pid(void) {
  int r = kill(-1, SIGTERM);
  TEST_ASSERT_TRUE(r < 0);
}

/* 2. sigaction register handler with new union struct */
void test_sigaction_register(void) {
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = SIG_IGN;

  int r = sigaction(SIGINT, &act, NULL);
  TEST_ASSERT_EQUAL_INT(0, r);

  /* Restore default */
  act.sa_handler = SIG_DFL;
  sigaction(SIGINT, &act, NULL);
}

/* 3. sigaction restore old handler */
void test_sigaction_restore(void) {
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = SIG_IGN;

  struct sigaction old_act;
  int r = sigaction(SIGUSR1, &act, &old_act);
  TEST_ASSERT_EQUAL_INT(0, r);

  /* old_act should contain previous handler (SIG_DFL) */
  TEST_ASSERT_EQUAL_PTR(SIG_DFL, old_act.sa_handler);

  /* Restore */
  sigaction(SIGUSR1, &old_act, NULL);
}

/* 4. sa_mask validation: SIGKILL/SIGSTOP cannot be masked */
void test_sigaction_mask_validation(void) {
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = SIG_IGN;
  act.sa_mask = (1ULL << SIGKILL); // illegal

  int r = sigaction(SIGUSR1, &act, NULL);
  TEST_ASSERT_TRUE(r < 0); // should fail

  /* Also test SIGSTOP */
  act.sa_mask = (1ULL << SIGSTOP);
  r = sigaction(SIGUSR1, &act, NULL);
  TEST_ASSERT_TRUE(r < 0);
}

/* 5. SIGKILL/SIGSTOP cannot be caught */
void test_sigkill_sigstop_catch(void) {
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = SIG_IGN;

  int r = sigaction(SIGKILL, &act, NULL);
  TEST_ASSERT_TRUE(r < 0);

  r = sigaction(SIGSTOP, &act, NULL);
  TEST_ASSERT_TRUE(r < 0);
}

/* 6. SA_SIGINFO flag can be set */
void test_sa_siginfo_flag(void) {
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_flags = SA_SIGINFO;
  act.sa_sigaction = NULL; // placeholder

  struct sigaction old_act;
  int r = sigaction(SIGUSR1, &act, &old_act);
  TEST_ASSERT_EQUAL_INT(0, r);

  /* Restore */
  sigaction(SIGUSR1, &old_act, NULL);
}

/* 7. signal() wrapper works with new union struct */
void test_signal_wrapper(void) {
  sighandler_t old = signal(SIGUSR2, SIG_IGN);
  TEST_ASSERT_EQUAL_PTR(SIG_DFL, old);

  old = signal(SIGUSR2, SIG_DFL);
  TEST_ASSERT_EQUAL_PTR(SIG_IGN, old);
}

/* 8. raise() sends signal to self */
void test_raise(void) {
  /* SIG_IGN: raise should succeed (signal ignored) */
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = SIG_IGN;
  sigaction(SIGUSR1, &act, NULL);

  int r = raise(SIGUSR1);
  TEST_ASSERT_EQUAL_INT(0, r);

  /* Restore */
  act.sa_handler = SIG_DFL;
  sigaction(SIGUSR1, &act, NULL);
}

/* 9. sigreturn restores context (volatile variable preserved) */
static volatile int sig_handler_called = 0;

static void sigusr1_handler(int sig) { sig_handler_called = sig; }

void test_sigreturn_restore(void) {
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = sigusr1_handler;

  struct sigaction old;
  sigaction(SIGUSR1, &act, &old);

  volatile int x = 42;
  raise(SIGUSR1);
  /* After sigreturn, x should still be 42 */
  TEST_ASSERT_EQUAL_INT(42, x);
  TEST_ASSERT_EQUAL_INT(SIGUSR1, sig_handler_called);

  /* Restore */
  sigaction(SIGUSR1, &old, NULL);
  sig_handler_called = 0;
}

/* 10. SIG_IGN prevents process termination */
void test_sigignore(void) {
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = SIG_IGN;
  sigaction(SIGTERM, &act, NULL);

  /* raise(SIGTERM) should not kill us because SIG_IGN */
  int r = raise(SIGTERM);
  TEST_ASSERT_EQUAL_INT(0, r);
  /* Process is still alive if we reach here */

  /* Restore */
  act.sa_handler = SIG_DFL;
  sigaction(SIGTERM, &act, NULL);
}

/* 11. Multi-process: SIGTERM kills child (uses pipe.elf to avoid recursive
 * signal.elf) */
void test_sigterm_child(void) {
  pid_t child = spawn_elf("/test/pipe.elf");
  if (child < 0) {
    TEST_FAIL_MESSAGE("spawn failed");
    return;
  }

  /* Wait briefly for child to start, then kill it */
  usleep(50000); // 50ms
  kill(child, SIGTERM);

  int status;
  pid_t ret = waitpid(child, &status, 0);
  TEST_ASSERT_EQUAL_INT(child, ret);
  /* Child terminated by signal */
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();
  RUN_TEST(test_kill_invalid_pid);
  RUN_TEST(test_sigaction_register);
  RUN_TEST(test_sigaction_restore);
  RUN_TEST(test_sigaction_mask_validation);
  RUN_TEST(test_sigkill_sigstop_catch);
  RUN_TEST(test_sa_siginfo_flag);
  RUN_TEST(test_signal_wrapper);
  RUN_TEST(test_raise);
  RUN_TEST(test_sigreturn_restore);
  RUN_TEST(test_sigignore);
  RUN_TEST(test_sigterm_child);
  return UNITY_END();
}

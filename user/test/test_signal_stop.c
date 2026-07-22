/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "user/test/test_helpers.h"
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

/* S01: raise(SIGSTOP) stops self; waitpid(WUNTRACED) reports WIFSTOPPED with
 * WSTOPSIG==SIGSTOP. Then SIGCONT resumes the child, which exits 7, and a
 * plain waitpid reports WIFEXITED / WEXITSTATUS==7. */
void test_stop_continue_wait(void) {
  pid_t child = fork();
  if (child == 0) {
    raise(SIGSTOP); /* stop self */
    _exit(7);
  }
  TEST_ASSERT_TRUE(child > 0);

  int status = 0;
  pid_t ret = waitpid(child, &status, WUNTRACED);
  TEST_ASSERT_EQUAL_INT(child, ret);
  TEST_ASSERT_TRUE(WIFSTOPPED(status));
  TEST_ASSERT_EQUAL_INT(SIGSTOP, WSTOPSIG(status));

  /* Resume the child. */
  TEST_ASSERT_EQUAL_INT(0, kill(child, SIGCONT));

  /* Now reap the exit. */
  status = 0;
  ret = waitpid(child, &status, 0);
  TEST_ASSERT_EQUAL_INT(child, ret);
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(7, WEXITSTATUS(status));
}

/* S01: default-action classification. SIGURG/SIGWINCH default to ignore (the
 * process must NOT die); SIGTSTP defaults to stop (WIFSTOPPED), not terminate.
 * Guards against the old bug where unlisted signals fell through to terminate.
 */
void test_default_action_ignore_and_stop(void) {
  /* SIGURG/SIGWINCH: default ignore → child survives, exits 3. */
  pid_t child = fork();
  if (child == 0) {
    raise(SIGURG);
    raise(SIGWINCH);
    _exit(3);
  }
  int status = 0;
  pid_t ret = waitpid(child, &status, 0);
  TEST_ASSERT_EQUAL_INT(child, ret);
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(3, WEXITSTATUS(status));

  /* SIGTSTP: default stop → reported via WUNTRACED, then SIGCONT + reap. */
  child = fork();
  if (child == 0) {
    raise(SIGTSTP);
    _exit(4);
  }
  status = 0;
  ret = waitpid(child, &status, WUNTRACED);
  TEST_ASSERT_EQUAL_INT(child, ret);
  TEST_ASSERT_TRUE(WIFSTOPPED(status));
  TEST_ASSERT_EQUAL_INT(SIGTSTP, WSTOPSIG(status));
  kill(child, SIGCONT);
  status = 0;
  ret = waitpid(child, &status, 0);
  TEST_ASSERT_EQUAL_INT(child, ret);
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(4, WEXITSTATUS(status));
}

/* S01: SIGKILL to a stopped child kills it (DA_TERM), reported as
 * WIFSIGNALED / WTERMSIG==SIGKILL. Guards the wake-to-exit path. */
void test_kill_stopped_child(void) {
  pid_t child = fork();
  if (child == 0) {
    raise(SIGSTOP);
    _exit(99); /* unreachable */
  }
  int status = 0;
  pid_t ret = waitpid(child, &status, WUNTRACED);
  TEST_ASSERT_EQUAL_INT(child, ret);
  TEST_ASSERT_TRUE(WIFSTOPPED(status));

  TEST_ASSERT_EQUAL_INT(0, kill(child, SIGKILL));
  status = 0;
  ret = waitpid(child, &status, 0);
  TEST_ASSERT_EQUAL_INT(child, ret);
  TEST_ASSERT_TRUE(WIFSIGNALED(status));
  TEST_ASSERT_EQUAL_INT(SIGKILL, WTERMSIG(status));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_stop_continue_wait);
  RUN_TEST(test_default_action_ignore_and_stop);
  RUN_TEST(test_kill_stopped_child);
  return UNITY_END();
}

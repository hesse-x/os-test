/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "user/test/test_helpers.h"
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <syscall.h> // SYS_RT_SIGRETURN
#include <unistd.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

/* S02: WCOREDUMP. Core-dump signals (SIGQUIT/SIGILL/SIGSEGV/...) set the 0x80
 * bit; SIGTERM does not. */
void test_wcoredump_set_and_clear(void) {
  pid_t child = fork();
  if (child == 0) {
    raise(SIGQUIT);
    _exit(0); /* unreachable */
  }
  int status = 0;
  pid_t ret = waitpid(child, &status, 0);
  TEST_ASSERT_EQUAL_INT(child, ret);
  TEST_ASSERT_TRUE(WIFSIGNALED(status));
  TEST_ASSERT_EQUAL_INT(SIGQUIT, WTERMSIG(status));
  TEST_ASSERT_TRUE(WCOREDUMP(status));

  child = fork();
  if (child == 0) {
    raise(SIGTERM);
    _exit(0); /* unreachable */
  }
  status = 0;
  ret = waitpid(child, &status, 0);
  TEST_ASSERT_EQUAL_INT(child, ret);
  TEST_ASSERT_TRUE(WIFSIGNALED(status));
  TEST_ASSERT_EQUAL_INT(SIGTERM, WTERMSIG(status));
  TEST_ASSERT_FALSE(WCOREDUMP(status));
}

/* S02: SA_RESETHAND. A handler installed with SA_RESETHAND fires once, then the
 * action resets to SIG_DFL — a second raise takes the default (terminate). */
static volatile int reset_handler_fired;
static void reset_handler(int sig) {
  (void)sig;
  reset_handler_fired = 1;
}

void test_sa_resethand(void) {
  reset_handler_fired = 0;
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = reset_handler;
  act.sa_flags = SA_RESETHAND;
  TEST_ASSERT_EQUAL_INT(0, sigaction(SIGUSR1, &act, NULL));

  raise(SIGUSR1);
  TEST_ASSERT_EQUAL_INT(1, reset_handler_fired);

  /* Second raise must now take SIG_DFL (terminate the process). Verify by
   * forking: the child sets RESETHAND, raises once (survives), then raises
   * again and dies by signal. */
  pid_t child = fork();
  if (child == 0) {
    struct sigaction a;
    memset(&a, 0, sizeof(a));
    a.sa_handler = reset_handler;
    a.sa_flags = SA_RESETHAND;
    sigaction(SIGUSR1, &a, NULL);
    raise(SIGUSR1); /* handler fires, action resets */
    raise(SIGUSR1); /* SIG_DFL → SIGUSR1 terminates */
    _exit(0);       /* unreachable */
  }
  int status = 0;
  pid_t ret = waitpid(child, &status, 0);
  TEST_ASSERT_EQUAL_INT(child, ret);
  TEST_ASSERT_TRUE(WIFSIGNALED(status));
  TEST_ASSERT_EQUAL_INT(SIGUSR1, WTERMSIG(status));
}

/* S02: SA_NODEFER. The signal is NOT auto-added to the blocked mask while the
 * handler runs, so inside the handler sigprocmask reports the current signal
 * as unblocked. Under the default (auto-defer) the kernel adds the current
 * signal to sig_blocked during delivery, so the handler sees it blocked.
 *
 * We observe the blocked mask (not a re-entrant raise): re-raising inside an
 * SA_NODEFER handler would re-deliver immediately and recurse without bound
 * (the kernel now survives the resulting stack overflow by forcing SIGSEGV
 * instead of panicking, but the test should not rely on that).
 *
 * (The S02 design note phrases this as "sigpending 含当前信号"; the observable
 * kernel behavior SA_NODEFER controls is the blocked-mask auto-add at
 * deliver_signal time — signal.c: new_mask |= (1<<sig) only when !SA_NODEFER —
 * so we assert on sigprocmask, which is the direct, non-recursive check.) */
static volatile int nodefer_sigusr1_blocked;
static volatile int nodefer_handler_fired;
static void nodefer_handler(int sig) {
  (void)sig;
  sigset_t cur;
  sigemptyset(&cur);
  sigprocmask(0, NULL,
              &cur); /* SIG_BLOCK=0 with NULL set just reads the mask */
  nodefer_sigusr1_blocked = sigismember(&cur, SIGUSR1);
  nodefer_handler_fired = 1;
}

static volatile int default_sigusr1_blocked;
static void default_nodefer_handler(int sig) {
  (void)sig;
  sigset_t cur;
  sigemptyset(&cur);
  sigprocmask(0, NULL, &cur);
  default_sigusr1_blocked = sigismember(&cur, SIGUSR1);
}

void test_sa_nodefer(void) {
  /* SA_NODEFER: SIGUSR1 stays unblocked inside the handler. */
  nodefer_sigusr1_blocked = -1;
  nodefer_handler_fired = 0;
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = nodefer_handler;
  act.sa_flags = SA_NODEFER;
  TEST_ASSERT_EQUAL_INT(0, sigaction(SIGUSR1, &act, NULL));
  raise(SIGUSR1);
  TEST_ASSERT_EQUAL_INT(1, nodefer_handler_fired);
  TEST_ASSERT_EQUAL_INT(0, nodefer_sigusr1_blocked);

  /* Default (auto-defer): SIGUSR1 IS blocked inside the handler. */
  default_sigusr1_blocked = -1;
  memset(&act, 0, sizeof(act));
  act.sa_handler = default_nodefer_handler;
  act.sa_flags = 0;
  TEST_ASSERT_EQUAL_INT(0, sigaction(SIGUSR1, &act, NULL));
  raise(SIGUSR1);
  TEST_ASSERT_EQUAL_INT(1, default_sigusr1_blocked);

  /* Restore default. */
  memset(&act, 0, sizeof(act));
  act.sa_handler = SIG_DFL;
  sigaction(SIGUSR1, &act, NULL);
}

/* S02: sa_restorer. The kernel must honor a user-supplied sa_restorer as the
 * signal return trampoline instead of the fixed SIG_TRAMPOLINE_ADDR page. We
 * install a restorer that calls rt_sigreturn and confirm the handler ran and
 * control returned to the instruction after raise(). */
static volatile int restorer_handler_fired;
static volatile int restorer_after_raise;
static void restorer_handler(int sig) {
  (void)sig;
  restorer_handler_fired = 1;
}

/* The restorer is invoked with the signal frame on stack; it must perform the
 * rt_sigreturn syscall to unwind the kernel-built frame. Mirrors glibc's
 * __restore_rt. */
__attribute__((naked)) static void restorer_fn(void) {
  __asm__ volatile("mov %0, %%rax\n"
                   "syscall\n" ::"i"(SYS_RT_SIGRETURN));
}

void test_sa_restorer(void) {
  /* Sanity: rt_sigreturn syscall number is available (xos/syscall_nums.h via
   * the libc headers). The naked restorer uses it directly. */
  restorer_handler_fired = 0;
  restorer_after_raise = 0;

  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = restorer_handler;
  act.sa_flags = SA_RESTORER;
  act.sa_restorer = restorer_fn;
  TEST_ASSERT_EQUAL_INT(0, sigaction(SIGUSR1, &act, NULL));

  raise(SIGUSR1);
  restorer_after_raise = 1; /* must be reached: restorer returned correctly */

  TEST_ASSERT_EQUAL_INT(1, restorer_handler_fired);
  TEST_ASSERT_EQUAL_INT(1, restorer_after_raise);

  /* Restore default. */
  memset(&act, 0, sizeof(act));
  act.sa_handler = SIG_DFL;
  sigaction(SIGUSR1, &act, NULL);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_wcoredump_set_and_clear);
  RUN_TEST(test_sa_resethand);
  RUN_TEST(test_sa_nodefer);
  RUN_TEST(test_sa_restorer);
  return UNITY_END();
}

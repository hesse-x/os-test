/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* S03 — kill/tgkill permission & scope + set_tid_address 64-bit + per-process
 * alarm. Verification test for the S03 kernel changes (all already implemented
 * in the working tree):
 *   - NSIG=65, RT signals (SIGRTMIN..SIGRTMAX) deliverable
 *   - sig==0 = existence + permission probe, no delivery
 *   - kill_permitted: same-uid or euid==0 (CAP_KILL); else -EPERM
 *   - kill(-1,...) / kill(-pgid,...) scope (init + sender excluded)
 *   - set_tid_address stores a full 64-bit user pointer (no pid_t truncation)
 *   - alarm() is per-process (thread-group shared): a sibling thread's alarm
 *     fires SIGALRM into the process and interrupts another thread's blocking
 *     read
 */

#include "user/test/test_helpers.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <syscall.h> // sys_set_tid_address
#include <unistd.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

/* The libc kill()/sigaction()/read() wrappers return -1 and set errno on
 * failure (they translate the kernel's negative errno). Asserting errno
 * directly is more precise than just "< 0". */
#define ASSERT_ERRNO(call, expected_err)                                       \
  do {                                                                         \
    errno = 0;                                                                 \
    TEST_ASSERT_EQUAL_INT(-1, (call));                                         \
    TEST_ASSERT_EQUAL_INT(expected_err, errno);                                \
  } while (0)

/* ---- 1. RT signal delivery (NSIG=65) ---- */
static volatile int rt_handler_fired;
static void rt_handler(int sig) { rt_handler_fired = sig; }

void test_kill_rt_signal(void) {
  /* SIGRTMIN is a real, deliverable signal now that NSIG=65 (was 33, which
   * rejected anything >= 33). Install a handler and self-signal. */
  rt_handler_fired = 0;
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = rt_handler;
  TEST_ASSERT_EQUAL_INT(0, sigaction(SIGRTMIN, &act, NULL));
  TEST_ASSERT_EQUAL_INT(0, kill(getpid(), SIGRTMIN));
  TEST_ASSERT_EQUAL_INT(SIGRTMIN, rt_handler_fired);

  /* High RT signal near SIGRTMAX works too. */
  rt_handler_fired = 0;
  TEST_ASSERT_EQUAL_INT(0, sigaction(SIGRTMAX, &act, NULL));
  TEST_ASSERT_EQUAL_INT(0, kill(getpid(), SIGRTMAX));
  TEST_ASSERT_EQUAL_INT(SIGRTMAX, rt_handler_fired);

  /* Out-of-range sig is rejected with -EINVAL (NSIG=65, so sig 65 invalid;
   * negative sig also invalid). */
  ASSERT_ERRNO(kill(getpid(), 65), EINVAL);
  ASSERT_ERRNO(kill(getpid(), -1), EINVAL);

  memset(&act, 0, sizeof(act));
  act.sa_handler = SIG_DFL;
  sigaction(SIGRTMIN, &act, NULL);
  sigaction(SIGRTMAX, &act, NULL);
}

/* ---- 2. sig==0 = existence + permission probe (no delivery) ---- */
static volatile int probe_handler_fired;
static void probe_handler(int sig) {
  (void)sig;
  probe_handler_fired = 1;
}

void test_kill_sig_zero_probe(void) {
  /* sig==0 to an existing pid returns 0 and delivers nothing. */
  probe_handler_fired = 0;
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = probe_handler;
  sigaction(SIGUSR1, &act, NULL);
  TEST_ASSERT_EQUAL_INT(0, kill(getpid(), 0));
  TEST_ASSERT_EQUAL_INT(0, probe_handler_fired); /* nothing delivered */

  /* sig==0 to a nonexistent pid returns -1/ESRCH. */
  ASSERT_ERRNO(kill(999999, 0), ESRCH);
  ASSERT_ERRNO(kill(999999, SIGUSR1), ESRCH);

  memset(&act, 0, sizeof(act));
  act.sa_handler = SIG_DFL;
  sigaction(SIGUSR1, &act, NULL);
}

/* ---- 3. permission: non-root cannot signal a different-uid process ---- */
/* Single-root OS: a child drops to uid 1000 via setuid(), then tries to kill
 * its root parent. kill_permitted sees euid != 0 and uid mismatch → -EPERM.
 * (Linux CAP_KILL is approximated by euid==0 here; S03 design records the
 * missing capability subsystem as tech debt.) */
void test_kill_permission_denied(void) {
  pid_t parent = getpid();
  pid_t child = fork();
  if (child == 0) {
    setuid(1000); /* drop root: uid == euid == 1000 */
    /* Parent is still uid 0; non-root child may not signal it. */
    int r = kill(parent, SIGKILL);
    _exit(r == -1 && errno == EPERM ? 0 : 1); /* 0 = got EPERM as expected */
  }
  int status = 0;
  TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status)); /* child saw EPERM */
}

/* Same-uid kill succeeds (sanity counterweight to the denial case). */
void test_kill_permission_same_uid(void) {
  pid_t child = fork();
  if (child == 0) {
    /* Root child signaling root parent: permitted. Use sig==0 so no delivery
     * actually happens (we only want to confirm permission granted). */
    int r = kill(getppid(), 0);
    _exit(r == 0 ? 0 : 1);
  }
  int status = 0;
  TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
}

/* ---- 4. kill(-1, 0): broadcast existence probe (safe: sig==0, no delivery)
 * ---- kill(-1, sig) with sig!=0 would broadcast to every process except init
 * and the sender — including the test_runner parent (pid 7) and any other live
 * test process, which would derail the harness. We only exercise the sig==0
 * existence probe (returns 0 because sendable processes exist). The
 * destructive broadcast is covered by code review of kill_all(), not run here.
 */
void test_kill_minus_one_probe(void) {
  TEST_ASSERT_EQUAL_INT(0, kill(-1, 0));
  /* kill(-1, 0) from a process with no other sendable peers would be -ESRCH,
   * but the test runner always has init + siblings, so 0 is the stable result.
   */
}

/* ---- 5. set_tid_address stores a 64-bit (high, >4GiB) user pointer ---- */
/* The user stack lives near 0x7fffffffe000 (>4GiB). A child sets its
 * clear_tid_addr to a stack variable's address and exits. With the old pid_t
 * truncation, do_exit's clear of the high address #PF'd and the child died by
 * signal; with the 64-bit fix the child exits cleanly. */
void test_set_tid_address_64bit(void) {
  pid_t child = fork();
  if (child == 0) {
    volatile int tid_word = 0x1234;
    uint64_t addr = (uint64_t)&tid_word;
    /* Sanity: the address is genuinely >4GiB (higher-half user stack region),
     * so a 32-bit truncation would corrupt it. */
    if (addr <= 0xFFFFFFFFULL)
      _exit(2);
    sys_set_tid_address(addr);
    _exit(0); /* do_exit writes 0 to addr + futex_wake; must not fault */
  }
  int status = 0;
  TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
}

/* ---- 6. alarm() is per-process: a sibling thread's alarm interrupts this
 *         thread's blocking read (proves the deadline lives in the shared
 *         signal_struct, not the arming thread's xtask) ---- */
static volatile int alarm_handler_fired;
static void alarm_handler(int sig) {
  (void)sig;
  alarm_handler_fired = 1;
}

static void *arm_alarm_thread(void *arg) {
  (void)arg;
  /* Thread B arms the process alarm. If alarm were per-thread (the pre-S03
   * behavior), only thread B would be a candidate for SIGALRM delivery and
   * thread A's blocking read would never be interrupted by it. */
  alarm(1);
  return NULL;
}

void test_alarm_per_process(void) {
  alarm_handler_fired = 0;

  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = alarm_handler;
  sigaction(SIGALRM, &act, NULL);

  int pfd[2];
  TEST_ASSERT_EQUAL_INT(0, pipe(pfd));

  pthread_t tid;
  TEST_ASSERT_EQUAL_INT(0, pthread_create(&tid, NULL, arm_alarm_thread, NULL));

  /* Thread A blocks reading from an empty pipe. The per-process alarm armed
   * by thread B must fire SIGALRM into the process and interrupt this read
   * (EINTR) within ~1s. */
  char buf[8];
  ssize_t r = read(pfd[0], buf, sizeof(buf));
  TEST_ASSERT_EQUAL_INT(-1, r);
  TEST_ASSERT_EQUAL_INT(EINTR, errno);
  TEST_ASSERT_EQUAL_INT(1, alarm_handler_fired);

  pthread_join(tid, NULL);
  close(pfd[0]);
  close(pfd[1]);

  alarm(0); /* disarm any lingering */
  memset(&act, 0, sizeof(act));
  act.sa_handler = SIG_DFL;
  sigaction(SIGALRM, &act, NULL);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_kill_rt_signal);
  RUN_TEST(test_kill_sig_zero_probe);
  RUN_TEST(test_kill_permission_denied);
  RUN_TEST(test_kill_permission_same_uid);
  RUN_TEST(test_kill_minus_one_probe);
  RUN_TEST(test_set_tid_address_64bit);
  RUN_TEST(test_alarm_per_process);
  return UNITY_END();
}

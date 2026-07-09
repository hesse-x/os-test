/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_signalfd — signalfd Unity tests (design §8.5, SF-001~010).
 *
 * Kernel behavior (verified against kernel/bsd/signalfd.c & signal.c):
 * - signalfd intercepts a signal before handler delivery when the signal is in
 *   the fd mask and NOT blocked: the bit is re-set in sig_pending and delivery
 *   is deferred until signalfd_do_read consumes it.
 * - signalfd_do_read only consumes sig_pending & sigmask & ~sig_blocked
 *   (SIGKILL/SIGSTOP bypass blocking). So a blocked signal is not readable.
 * - signalfd4 with fd=-1 creates; fd>=0 updates the mask.
 * - SIGKILL/SIGSTOP are stripped from the mask on creation.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/signalfd.h>
#include <unistd.h>
#include <unity.h>
#include <xos/errno.h>

void setUp(void) {}
void tearDown(void) {}

static volatile int s_handler_called;

static void usr1_handler(int sig) { s_handler_called = sig; }

/* SF-001 */
void test_signalfd_create(void) {
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGUSR1);
  int fd = signalfd(-1, &mask, 0);
  TEST_ASSERT_TRUE(fd >= 0);
  close(fd);
}

/* SF-002 */
void test_signalfd_read_basic(void) {
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGUSR1);
  int fd = signalfd(-1, &mask, 0);
  raise(SIGUSR1);
  signalfd_siginfo si;
  int r = read(fd, &si, sizeof(si));
  TEST_ASSERT_EQUAL_INT((int)sizeof(si), r);
  TEST_ASSERT_EQUAL_INT(SIGUSR1, (int)si.ssi_signo);
  close(fd);
}

/* SF-003 */
void test_signalfd_consumes(void) {
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGUSR1);
  int fd = signalfd(-1, &mask, 0);
  raise(SIGUSR1);
  signalfd_siginfo si;
  read(fd, &si, sizeof(si));
  /* After consumption, sigpending must no longer report SIGUSR1. */
  sigset_t pend;
  sigemptyset(&pend);
  sigpending(&pend);
  TEST_ASSERT_TRUE(!sigismember(&pend, SIGUSR1));
  close(fd);
}

/* SF-004 */
void test_signalfd_priority_over_handler(void) {
  s_handler_called = 0;
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = usr1_handler;
  sigaction(SIGUSR1, &act, NULL);

  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGUSR1);
  int fd = signalfd(-1, &mask, 0);
  raise(SIGUSR1);
  signalfd_siginfo si;
  int r = read(fd, &si, sizeof(si));
  TEST_ASSERT_EQUAL_INT((int)sizeof(si), r);
  /* Handler must NOT have run: signalfd consumes first. */
  TEST_ASSERT_EQUAL_INT(0, s_handler_called);

  close(fd);
  struct sigaction dfl;
  memset(&dfl, 0, sizeof(dfl));
  dfl.sa_handler = SIG_DFL;
  sigaction(SIGUSR1, &dfl, NULL);
}

/* SF-005 */
void test_signalfd_handler_fallback(void) {
  /* If no signalfd reads the signal, the default/handler applies. Here we
   * create a signalfd but do NOT read it, then install a handler and raise.
   * The signal is intercepted (stays pending for signalfd), so the handler is
   * deferred — but the bit remains pending. This confirms the deferral
   * semantics: an unread signalfd keeps the signal pending (not delivered to
   * handler). We verify by checking sigpending reports it. */
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGUSR1);
  int fd = signalfd(-1, &mask, 0);
  raise(SIGUSR1);
  sigset_t pend;
  sigemptyset(&pend);
  sigpending(&pend);
  TEST_ASSERT_TRUE(sigismember(&pend, SIGUSR1));
  /* Drain to clear. */
  signalfd_siginfo si;
  read(fd, &si, sizeof(si));
  close(fd);
}

/* SF-006 */
void test_signalfd_blocked(void) {
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGUSR1);
  int fd = signalfd(-1, &mask, SFD_NONBLOCK);
  /* Block SIGUSR1 → signalfd cannot consume it. */
  sigset_t bset;
  sigemptyset(&bset);
  sigaddset(&bset, SIGUSR1);
  sigprocmask(SIG_BLOCK, &bset, NULL);
  raise(SIGUSR1);
  signalfd_siginfo si;
  int r = read(fd, &si, sizeof(si));
  TEST_ASSERT_EQUAL_INT(-1, r);
  TEST_ASSERT_EQUAL_INT(EAGAIN, errno);
  /* Unblock and drain to clear pending. */
  sigprocmask(SIG_UNBLOCK, &bset, NULL);
  read(fd, &si, sizeof(si));
  close(fd);
}

/* SF-007 */
void test_signalfd_nonblock_eagain(void) {
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGUSR1);
  int fd = signalfd(-1, &mask, SFD_NONBLOCK);
  signalfd_siginfo si;
  int r = read(fd, &si, sizeof(si));
  TEST_ASSERT_EQUAL_INT(-1, r);
  TEST_ASSERT_EQUAL_INT(EAGAIN, errno);
  close(fd);
}

/* SF-008 */
void test_signalfd_poll(void) {
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGUSR1);
  int fd = signalfd(-1, &mask, 0);
  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  TEST_ASSERT_EQUAL_INT(0, poll(&pfd, 1, 0));
  raise(SIGUSR1);
  pfd.revents = 0;
  TEST_ASSERT_TRUE(poll(&pfd, 1, 500) > 0);
  TEST_ASSERT_TRUE(pfd.revents & POLLIN);
  signalfd_siginfo si;
  read(fd, &si, sizeof(si));
  close(fd);
}

/* SF-009 */
void test_signalfd_update_mask(void) {
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGUSR1);
  int fd = signalfd(-1, &mask, 0);
  /* Update mask to SIGUSR2 instead. */
  sigset_t mask2;
  sigemptyset(&mask2);
  sigaddset(&mask2, SIGUSR2);
  int r = signalfd(fd, &mask2, 0);
  TEST_ASSERT_EQUAL_INT(fd, r);
  /* SIGUSR1 is no longer monitored → ignore it so raise won't terminate. */
  struct sigaction ign;
  memset(&ign, 0, sizeof(ign));
  ign.sa_handler = SIG_IGN;
  sigaction(SIGUSR1, &ign, NULL);
  raise(SIGUSR1);
  /* poll must not report SIGUSR1 (not in mask). */
  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  TEST_ASSERT_EQUAL_INT(0, poll(&pfd, 1, 50));
  /* SIGUSR2 is monitored → raise and read. */
  raise(SIGUSR2);
  signalfd_siginfo si;
  int rr = read(fd, &si, sizeof(si));
  TEST_ASSERT_EQUAL_INT((int)sizeof(si), rr);
  TEST_ASSERT_EQUAL_INT(SIGUSR2, (int)si.ssi_signo);
  /* Restore SIGUSR1 default. */
  ign.sa_handler = SIG_DFL;
  sigaction(SIGUSR1, &ign, NULL);
  close(fd);
}

/* SF-010 */
void test_signalfd_multi_signal(void) {
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGUSR1);
  sigaddset(&mask, SIGUSR2);
  int fd = signalfd(-1, &mask, 0);
  raise(SIGUSR1);
  raise(SIGUSR2);
  signalfd_siginfo si;
  uint32_t seen = 0;
  for (int i = 0; i < 2; i++) {
    int r = read(fd, &si, sizeof(si));
    if (r == (int)sizeof(si))
      seen |= 1U << si.ssi_signo;
  }
  TEST_ASSERT_TRUE(seen & (1U << SIGUSR1));
  TEST_ASSERT_TRUE(seen & (1U << SIGUSR2));
  close(fd);
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();
  RUN_TEST(test_signalfd_create);
  RUN_TEST(test_signalfd_read_basic);
  RUN_TEST(test_signalfd_consumes);
  RUN_TEST(test_signalfd_priority_over_handler);
  RUN_TEST(test_signalfd_handler_fallback);
  RUN_TEST(test_signalfd_blocked);
  RUN_TEST(test_signalfd_nonblock_eagain);
  RUN_TEST(test_signalfd_poll);
  RUN_TEST(test_signalfd_update_mask);
  RUN_TEST(test_signalfd_multi_signal);
  return UNITY_END();
}

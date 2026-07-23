/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_epoll_oneshot — S18 EPOLLONESHOT / EPOLLEXCLUSIVE / epoll_ctl self-reg.
 *
 * Covers (refact_syscall/S18_epoll_oneshot_accept.md):
 * - EPOLLONESHOT: report once then auto-disarm; no re-report until MOD re-arm.
 * - EPOLL_CTL_MOD re-arms a disarmed oneshot (clears is_disarmed).
 * - EPOLLEXCLUSIVE: among multiple epolls on one fd, a single wake wakes only
 *   one exclusive waiter (anti-thundering-herd). Verified by prefork: two
 *   children each epoll_wait the same pipe; one byte written → exactly one
 *   child's epoll_wait returns 1, the other returns 0 (timeout).
 * - epoll_ctl(epfd, ADD, epfd, ...) self-registration → -1/EINVAL.
 * - EPOLLEXCLUSIVE toggle on MOD → -1/EINVAL (Linux: ADD-only).
 * - ET + ONESHOT combo still reports once.
 *
 * Conventions match test_epoll.c (Unity, fork+waitpid, alarm/SIGALRM).
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/process.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

/* ===================== EPOLLONESHOT ===================== */

/* ONESHOT reports once; a second epoll_wait (no MOD) returns 0. */
void test_oneshot_report_once(void) {
  int fd[2];
  pipe(fd);
  int ep = epoll_create1(0);
  TEST_ASSERT_TRUE(ep >= 0);
  struct epoll_event ev = {.events = EPOLLIN | EPOLLONESHOT, .data.fd = fd[0]};
  TEST_ASSERT_EQUAL_INT(0, epoll_ctl(ep, EPOLL_CTL_ADD, fd[0], &ev));
  write(fd[1], "x", 1);

  struct epoll_event out[4];
  int n1 = epoll_wait(ep, out, 4, 100);
  TEST_ASSERT_EQUAL_INT(1, n1);
  TEST_ASSERT_TRUE(out[0].events & EPOLLIN);

  /* Data still unread, but ONESHOT disarmed → no re-report. */
  int n2 = epoll_wait(ep, out, 4, 100);
  TEST_ASSERT_EQUAL_INT(0, n2);

  close(fd[0]);
  close(fd[1]);
  close(ep);
}

/* EPOLL_CTL_MOD re-arms a disarmed ONESHOT. */
void test_oneshot_mod_rearm(void) {
  int fd[2];
  pipe(fd);
  int ep = epoll_create1(0);
  struct epoll_event ev = {.events = EPOLLIN | EPOLLONESHOT, .data.fd = fd[0]};
  epoll_ctl(ep, EPOLL_CTL_ADD, fd[0], &ev);
  write(fd[1], "x", 1);
  struct epoll_event out[4];
  TEST_ASSERT_EQUAL_INT(1, epoll_wait(ep, out, 4, 100));
  TEST_ASSERT_EQUAL_INT(0, epoll_wait(ep, out, 4, 100)); // disarmed

  /* Re-arm with the same ONESHOT mask; data still pending → reports again. */
  ev.events = EPOLLIN | EPOLLONESHOT;
  TEST_ASSERT_EQUAL_INT(0, epoll_ctl(ep, EPOLL_CTL_MOD, fd[0], &ev));
  int n = epoll_wait(ep, out, 4, 100);
  TEST_ASSERT_EQUAL_INT(1, n);
  TEST_ASSERT_TRUE(out[0].events & EPOLLIN);

  close(fd[0]);
  close(fd[1]);
  close(ep);
}

/* ONESHOT may be turned off via MOD (no ONESHOT bit) → back to LT persistence.
 */
void test_oneshot_mod_clear(void) {
  int fd[2];
  pipe(fd);
  int ep = epoll_create1(0);
  struct epoll_event ev = {.events = EPOLLIN | EPOLLONESHOT, .data.fd = fd[0]};
  epoll_ctl(ep, EPOLL_CTL_ADD, fd[0], &ev);
  write(fd[1], "x", 1);
  struct epoll_event out[4];
  TEST_ASSERT_EQUAL_INT(1, epoll_wait(ep, out, 4, 100));
  TEST_ASSERT_EQUAL_INT(0, epoll_wait(ep, out, 4, 100)); // disarmed

  /* MOD drops ONESHOT → LT: reports persistently until consumed. */
  ev.events = EPOLLIN;
  TEST_ASSERT_EQUAL_INT(0, epoll_ctl(ep, EPOLL_CTL_MOD, fd[0], &ev));
  TEST_ASSERT_TRUE(epoll_wait(ep, out, 4, 100) >= 1);
  TEST_ASSERT_TRUE(epoll_wait(ep, out, 4, 100) >= 1);

  close(fd[0]);
  close(fd[1]);
  close(ep);
}

/* ET + ONESHOT: edge reports once, ONESHOT keeps it disarmed. */
void test_oneshot_et_combo(void) {
  int fd[2];
  pipe(fd);
  int ep = epoll_create1(0);
  struct epoll_event ev = {.events = EPOLLIN | EPOLLET | EPOLLONESHOT,
                           .data.fd = fd[0]};
  epoll_ctl(ep, EPOLL_CTL_ADD, fd[0], &ev);
  write(fd[1], "x", 1);
  struct epoll_event out[4];
  TEST_ASSERT_EQUAL_INT(1, epoll_wait(ep, out, 4, 100));
  /* Second write while disarmed → must not re-report (ONESHOT holds). */
  write(fd[1], "y", 1);
  TEST_ASSERT_EQUAL_INT(0, epoll_wait(ep, out, 4, 100));
  close(fd[0]);
  close(fd[1]);
  close(ep);
}

/* ===================== EPOLLEXCLUSIVE (anti-thundering-herd)
 * ===================== */

/* Two children each epoll the same pipe read end with EPOLLEXCLUSIVE. Parent
 * writes one byte; exactly one child's epoll_wait returns 1, the other 0.
 * Children report their count back via a pipe. */
void test_exclusive_one_wakes(void) {
  int pfd[2];
  pipe(pfd);

  /* Each child writes its result (1 if it saw the event, else 0) here. */
  int rep_a[2], rep_b[2];
  pipe(rep_a);
  pipe(rep_b);

  pid_t pa = fork();
  if (pa == 0) {
    int ep = epoll_create1(0);
    struct epoll_event ev = {.events = EPOLLIN | EPOLLEXCLUSIVE};
    epoll_ctl(ep, EPOLL_CTL_ADD, pfd[0], &ev);
    struct epoll_event out[2];
    int n = epoll_wait(ep, out, 2, 500);
    int got = (n >= 1) ? 1 : 0;
    write(rep_a[1], &got, sizeof(got));
    close(rep_a[1]);
    _exit(0);
  }
  pid_t pb = fork();
  if (pb == 0) {
    int ep = epoll_create1(0);
    struct epoll_event ev = {.events = EPOLLIN | EPOLLEXCLUSIVE};
    epoll_ctl(ep, EPOLL_CTL_ADD, pfd[0], &ev);
    struct epoll_event out[2];
    int n = epoll_wait(ep, out, 2, 500);
    int got = (n >= 1) ? 1 : 0;
    write(rep_b[1], &got, sizeof(got));
    close(rep_b[1]);
    _exit(0);
  }

  /* Let both children reach epoll_wait before signalling. */
  usleep(100 * 1000);
  write(pfd[1], "x", 1);

  int ga = 0, gb = 0;
  read(rep_a[0], &ga, sizeof(ga));
  read(rep_b[0], &gb, sizeof(gb));
  int total = ga + gb;
  /* Exactly one exclusive waiter should be woken by the single byte. */
  TEST_ASSERT_EQUAL_INT(1, total);

  int st;
  waitpid(pa, &st, 0);
  waitpid(pb, &st, 0);

  close(pfd[0]);
  close(pfd[1]);
  close(rep_a[0]);
  close(rep_a[1]);
  close(rep_b[0]);
  close(rep_b[1]);
}

/* ===================== epoll_ctl self-registration ===================== */

/* epoll_ctl(epfd, ADD, epfd, ...) → -1/EINVAL (reject self-registration). */
void test_ctl_self_register_einval(void) {
  int ep = epoll_create1(0);
  struct epoll_event ev = {.events = EPOLLIN};
  int r = epoll_ctl(ep, EPOLL_CTL_ADD, ep, &ev);
  TEST_ASSERT_EQUAL_INT(-1, r);
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
  close(ep);
}

/* epoll_ctl(epfd, MOD, epfd, ...) → -1/EINVAL too. */
void test_ctl_self_mod_einval(void) {
  int ep = epoll_create1(0);
  struct epoll_event ev = {.events = EPOLLIN};
  int r = epoll_ctl(ep, EPOLL_CTL_MOD, ep, &ev);
  TEST_ASSERT_EQUAL_INT(-1, r);
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
  close(ep);
}

/* ===================== EPOLLEXCLUSIVE MOD toggle → EINVAL
 * ===================== */

/* ADD with EXCLUSIVE then MOD that drops it → -1/EINVAL (Linux: ADD-only). */
void test_exclusive_mod_toggle_einval(void) {
  int fd[2];
  pipe(fd);
  int ep = epoll_create1(0);
  struct epoll_event ev = {.events = EPOLLIN | EPOLLEXCLUSIVE,
                           .data.fd = fd[0]};
  TEST_ASSERT_EQUAL_INT(0, epoll_ctl(ep, EPOLL_CTL_ADD, fd[0], &ev));
  ev.events = EPOLLIN; /* drop EXCLUSIVE on MOD */
  int r = epoll_ctl(ep, EPOLL_CTL_MOD, fd[0], &ev);
  TEST_ASSERT_EQUAL_INT(-1, r);
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
  close(fd[0]);
  close(fd[1]);
  close(ep);
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();
  RUN_TEST(test_oneshot_report_once);
  RUN_TEST(test_oneshot_mod_rearm);
  RUN_TEST(test_oneshot_mod_clear);
  RUN_TEST(test_oneshot_et_combo);
  RUN_TEST(test_exclusive_one_wakes);
  RUN_TEST(test_ctl_self_register_einval);
  RUN_TEST(test_ctl_self_mod_einval);
  RUN_TEST(test_exclusive_mod_toggle_einval);
  return UNITY_END();
}

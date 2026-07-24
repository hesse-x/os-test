/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* wait4 extension flags __WALL / __WCLONE / __WNOTHREAD.
 *
 * This kernel has no thread-group vs clone-child distinction (child_matches
 * keys on signal->parent_pid, so every child already matches), hence:
 *   - __WALL       : no-op (already matches all children) → reaps normally
 *   - __WNOTHREAD  : no-op (wait never crosses threads)   → reaps normally
 *   - __WCLONE     : accepted, treated like __WALL        → reaps normally
 *   - unknown bits : rejected with -EINVAL (whitelist tightening, mirrors
 *                    Linux do_wait)
 *
 * We call wait4 directly (raw syscall) so the libc waitpid wrapper does not
 * discard the extension flags. */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/process.h> // fork
#include <sys/wait.h>
#include <syscall.h> // __syscall4, SYS_WAIT4
#include <unistd.h>
#include <unity.h>
#include <xos/errno.h>

void setUp(void) {}
void tearDown(void) {}

static int64_t wait4_raw(int pid, int *wstatus, int options) {
  return __syscall4(SYS_WAIT4, (int64_t)pid, (int64_t)(uintptr_t)wstatus,
                    (int64_t)options, 0);
}

/* Spawn a child that exits with `code`; return its pid. */
static pid_t spawn_child(int code) {
  pid_t pid = fork();
  if (pid == 0)
    _exit(code);
  return pid;
}

/* Reap via wait4 with `options`; assert the reaped pid + exit status. */
static void reap_with_options(int options, int expect_code) {
  pid_t child = spawn_child(expect_code);
  TEST_ASSERT_TRUE(child > 0);

  int status = 0;
  int64_t r = wait4_raw(child, &status, options);
  TEST_ASSERT_EQUAL_INT(child, r);
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(expect_code, WEXITSTATUS(status));
}

/* ---- 1. __WALL reaps a child normally ---- */
void test_wait4_wall_reaps(void) { reap_with_options(__WALL, 11); }

/* ---- 2. __WNOTHREAD reaps a child normally (no-op here) ---- */
void test_wait4_wnothread_reaps(void) { reap_with_options(__WNOTHREAD, 22); }

/* ---- 3. __WCLONE accepted, behaves like __WALL ---- */
void test_wait4_wclone_reaps(void) { reap_with_options(__WCLONE, 33); }

/* ---- 4. __WALL|WNOHANG composes: child not ready → 0, then reap ---- */
void test_wait4_wall_compose_wnohang(void) {
  int pfd[2];
  TEST_ASSERT_EQUAL_INT(0, pipe(pfd));

  pid_t child = fork();
  if (child == 0) {
    close(pfd[1]);
    char c;
    /* Block until parent releases us. */
    while (read(pfd[0], &c, 1) != 1)
      ;
    _exit(0);
  }
  close(pfd[0]);
  TEST_ASSERT_TRUE(child > 0);

  /* Child still running (blocked in read) → WNOHANG|__WALL returns 0. */
  int status = 0;
  int64_t r = wait4_raw(child, &status, WNOHANG | __WALL);
  TEST_ASSERT_EQUAL_INT(0, r);

  /* Release + reap. */
  char c = 1;
  TEST_ASSERT_EQUAL_INT(1, write(pfd[1], &c, 1));
  close(pfd[1]);
  r = wait4_raw(child, &status, __WALL);
  TEST_ASSERT_EQUAL_INT(child, r);
  TEST_ASSERT_TRUE(WIFEXITED(status));
}

/* ---- 5. unknown option bit rejected with -EINVAL ---- */
void test_wait4_unknown_option_einval(void) {
  pid_t child = spawn_child(0);
  TEST_ASSERT_TRUE(child > 0);

  /* 0x80 is not in __W_KNOWN. */
  int status = 0;
  int64_t r = wait4_raw(child, &status, 0x80);
  TEST_ASSERT_EQUAL_INT(-EINVAL, r);

  /* Child still alive (not reaped) → reap it. */
  r = wait4_raw(child, &status, 0);
  TEST_ASSERT_EQUAL_INT(child, r);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_wait4_wall_reaps);
  RUN_TEST(test_wait4_wnothread_reaps);
  RUN_TEST(test_wait4_wclone_reaps);
  RUN_TEST(test_wait4_wall_compose_wnohang);
  RUN_TEST(test_wait4_unknown_option_einval);
  return UNITY_END();
}

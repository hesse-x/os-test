/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* S19 §5 — wait4 pid semantics (pid==0 own pgroup, pid<-1 by |pgid|) +
 * rusage + mismatch. Verification test for the kernel change (already
 * implemented):
 *   - wait4(0, ...) reaps a child whose pgid == caller's pgid
 *   - wait4(-pgid, ...) reaps a child whose pgid == |pid|
 *   - rusage (non-NULL) is filled from the child's cpu_time_ns: ru_utime>0,
 *     ru_stime==0, ru_maxrss==0, rest zeroed (only utime tracked today)
 *   - rusage == NULL still works (the main path is unaffected)
 *   - a child in a DIFFERENT pgroup is not matched by wait4(0): with WNOHANG it
 *     returns -ECHILD (no child in the caller's group)
 *
 * We call wait4 directly (not the libc waitpid, which always passes
 * rusage=NULL). Race-free synchronization uses a pipe (inherited across fork)
 * rather than shared memory: the child signals "setpgid done" before the
 * parent probes, so a mismatch verdict is deterministic. */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/process.h>  // fork, setpgid, getpgid
#include <sys/resource.h> // struct rusage
#include <sys/wait.h>
#include <syscall.h> // __syscall4, SYS_WAIT4
#include <unistd.h>
#include <unity.h>
#include <xos/errno.h>
#include <xos/time.h> // struct timeval (must precede <sys/resource.h>)

void setUp(void) {}
void tearDown(void) {}

/* Raw wait4: returns reaped pid (>0), 0 (WNOHANG, none ready), or negative
 * errno. `ru` may be NULL. */
static int64_t wait4_raw(int pid, int *wstatus, int options,
                         struct rusage *ru) {
  return __syscall4(SYS_WAIT4, (int64_t)pid, (int64_t)(uintptr_t)wstatus,
                    (int64_t)options, (int64_t)(uintptr_t)ru);
}

/* Burn CPU so the child accumulates nonzero cpu_time_ns → ru_utime>0. */
static void burn_cpu(void) {
  volatile long acc = 0;
  for (long i = 0; i < 3000000L; i++)
    acc += i;
  (void)acc;
}

/* Wait until the child writes one byte to the pipe (its "ready" signal). */
static void wait_child_ready(int rfd) {
  char c;
  while (read(rfd, &c, 1) != 1)
    sched_yield();
}

/* ---- 1. wait4(0): reap a child in the caller's own pgroup ---- */
void test_wait4_pgid_zero(void) {
  TEST_ASSERT_EQUAL_INT(0, setpgid(0, 0)); /* become own group leader */

  pid_t child = fork();
  if (child == 0) {
    burn_cpu();
    _exit(7);
  }
  TEST_ASSERT_TRUE(child > 0);

  int status = 0;
  struct rusage ru;
  memset(&ru, 0, sizeof(ru));
  int64_t r = wait4_raw(0, &status, 0, &ru);
  TEST_ASSERT_EQUAL_INT(child, r);
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(7, WEXITSTATUS(status));
  /* rusage contract: only ru_utime populated, rest zero. */
  TEST_ASSERT_TRUE(ru.ru_utime.tv_sec > 0 || ru.ru_utime.tv_usec > 0);
  TEST_ASSERT_EQUAL_INT(0, ru.ru_stime.tv_sec);
  TEST_ASSERT_EQUAL_INT(0, ru.ru_stime.tv_usec);
  TEST_ASSERT_EQUAL_INT(0, ru.ru_maxrss);
  TEST_ASSERT_EQUAL_INT(0, ru.ru_nvcsw + ru.ru_minflt);
}

/* ---- 2. wait4(-pgid): reap a child by process group id ---- */
void test_wait4_pgid_negative(void) {
  TEST_ASSERT_EQUAL_INT(0, setpgid(0, 0));
  pid_t my_pgid = getpgid(0);

  pid_t child = fork();
  if (child == 0) {
    _exit(42);
  }
  TEST_ASSERT_TRUE(child > 0);

  int status = 0;
  int64_t r = wait4_raw(-(int)my_pgid, &status, 0, NULL);
  TEST_ASSERT_EQUAL_INT(child, r);
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(42, WEXITSTATUS(status));
}

/* ---- 3. mismatch: child in a different group is not matched by wait4(0) ----
 * The child setpgid's into another group, then signals readiness over a pipe.
 * The parent then probes with WNOHANG: its own group has no child, so wait4
 * returns -ECHILD (a determined value, not a race). The child is then reaped
 * by pid to avoid a lingering zombie. */
void test_wait4_pgid_mismatch(void) {
  TEST_ASSERT_EQUAL_INT(0, setpgid(0, 0));

  int pfd[2];
  TEST_ASSERT_EQUAL_INT(0, pipe(pfd));

  pid_t child = fork();
  if (child == 0) {
    close(pfd[0]);
    /* Move into a different group (a new pgid == our pid). */
    setpgid(0, 0);
    char c = 1;
    write(pfd[1], &c, 1);
    close(pfd[1]);
    _exit(0);
  }
  close(pfd[1]);
  TEST_ASSERT_TRUE(child > 0);

  wait_child_ready(pfd[0]);
  close(pfd[0]);

  /* Now the child is in its own group, not the parent's → wait4(0) finds no
   * matching child. The kernel returns -ECHILD (not 0): it scans, sees the
   * child is not in caller's group, and has_children is false for that group.
   */
  int status = 0;
  int64_t r = wait4_raw(0, &status, WNOHANG, NULL);
  TEST_ASSERT_EQUAL_INT(-ECHILD, r);

  /* Reap by explicit pid so the child does not linger. */
  TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
}

/* ---- 4. rusage for an arbitrary child (pid==-1) is filled ---- */
void test_wait4_rusage_any_child(void) {
  pid_t child = fork();
  if (child == 0) {
    burn_cpu();
    _exit(0);
  }
  TEST_ASSERT_TRUE(child > 0);

  int status = 0;
  struct rusage ru;
  memset(&ru, 0, sizeof(ru));
  int64_t r = wait4_raw(-1, &status, 0, &ru);
  TEST_ASSERT_EQUAL_INT(child, r);
  TEST_ASSERT_TRUE(WIFEXITED(status));
  /* ru_utime must be a valid timeval (usec < 1e6) and nonzero after burn. */
  TEST_ASSERT_TRUE(ru.ru_utime.tv_usec >= 0 && ru.ru_utime.tv_usec < 1000000);
  TEST_ASSERT_TRUE(ru.ru_utime.tv_sec > 0 || ru.ru_utime.tv_usec > 0);
  TEST_ASSERT_EQUAL_INT(0, ru.ru_stime.tv_sec);
  TEST_ASSERT_EQUAL_INT(0, ru.ru_maxrss);
}

/* ---- 5. rusage==NULL does not break the main reap path ---- */
void test_wait4_no_rusage_still_works(void) {
  pid_t child = fork();
  if (child == 0)
    _exit(9);
  TEST_ASSERT_TRUE(child > 0);

  int status = 0;
  int64_t r = wait4_raw(-1, &status, 0, NULL);
  TEST_ASSERT_EQUAL_INT(child, r);
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(9, WEXITSTATUS(status));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_wait4_pgid_zero);
  RUN_TEST(test_wait4_pgid_negative);
  RUN_TEST(test_wait4_pgid_mismatch);
  RUN_TEST(test_wait4_rusage_any_child);
  RUN_TEST(test_wait4_no_rusage_still_works);
  return UNITY_END();
}

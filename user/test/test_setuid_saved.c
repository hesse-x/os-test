/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* S19 §6 — setuid/setgid saved-set (suid/sgid) + Linux permission ladder.
 * Verification test for the kernel change (already implemented):
 *   - euid==0 (root): setuid sets real + effective + saved-set all to uid
 *   - euid!=0: setuid may only set euid to the current real uid or saved-set
 *     uid; anything else → -EPERM (no privilege escalation)
 *   - after dropping root (suid becomes the dropped uid), setuid(0) fails —
 *     the saved-set no longer holds root, so you cannot climb back
 *   - setgid mirrors this over gid/egid/sgid
 *
 * Dropping root is irreversible within a process, so every case forks into a
 * fresh child (which starts as root, inherits nothing stale), runs the ladder
 * steps, and exits 0/1 to signal pass/fail. The parent stays root throughout
 * and asserts the child's exit status. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/process.h> // fork, setuid, setgid
#include <sys/wait.h>
#include <unistd.h>
#include <unity.h>
#include <xos/errno.h>

void setUp(void) {}
void tearDown(void) {}

#define FAIL_IF(cond)                                                          \
  do {                                                                         \
    if (cond)                                                                  \
      _exit(1);                                                                \
  } while (0)

/* ---- 1. root setuid sets all three; non-root ladder obeys uid/suid ---- */
/* child: setuid(1000) → uid=euid=suid=1000. Then setuid(0) must fail (-EPERM)
 * because 0 is neither the real uid (1000) nor the saved-set (1000). Then
 * setuid(1000) succeeds (euid already 1000, but it equals real → allowed). */
void test_setuid_root_drop_then_eperm(void) {
  pid_t child = fork();
  if (child == 0) {
    FAIL_IF(setuid(1000) != 0);
    FAIL_IF(getuid() != 1000 || geteuid() != 1000);
    /* suid must also be 1000 (root branch set all three); climbing back to 0
     * is now forbidden. */
    if (setuid(0) != -1 || errno != EPERM)
      _exit(1);
    /* setuid(1000) again: 1000 == real uid → allowed. */
    FAIL_IF(setuid(1000) != 0);
    _exit(0);
  }
  int status = 0;
  TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
}

/* ---- 2. after drop, setuid(0) fails (suid truly dropped, not retained 0) ----
 * This is the regression guard: if the root branch forgot to set suid, suid
 * would stay 0 and setuid(0) would wrongly succeed. */
void test_setuid_suid_dropped(void) {
  pid_t child = fork();
  if (child == 0) {
    FAIL_IF(setuid(1000) != 0);
    if (setuid(0) != -1 || errno != EPERM)
      _exit(1);
    _exit(0);
  }
  int status = 0;
  TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
}

/* ---- 3. setgid mirrors setuid over gid/egid/sgid ---- */
void test_setgid_ladder(void) {
  pid_t child = fork();
  if (child == 0) {
    FAIL_IF(setgid(1000) != 0);
    FAIL_IF(getgid() != 1000 || getegid() != 1000);
    if (setgid(0) != -1 || errno != EPERM)
      _exit(1);
    FAIL_IF(setgid(1000) != 0);
    _exit(0);
  }
  int status = 0;
  TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
}

/* ---- 4. root branch sets euid too: setuid(1000) then setuid(0) fails ----
 * Confirms the root branch did not leave euid==0 (which would let setuid(0)
 * succeed). After setuid(1000), euid is 1000, so the next setuid(0) is judged
 * by the non-root ladder and fails. */
void test_setuid_root_sets_euid(void) {
  pid_t child = fork();
  if (child == 0) {
    FAIL_IF(setuid(1000) != 0);
    FAIL_IF(geteuid() != 1000);
    if (setuid(0) != -1 || errno != EPERM)
      _exit(1);
    _exit(0);
  }
  int status = 0;
  TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_setuid_root_drop_then_eperm);
  RUN_TEST(test_setuid_suid_dropped);
  RUN_TEST(test_setgid_ladder);
  RUN_TEST(test_setuid_root_sets_euid);
  return UNITY_END();
}

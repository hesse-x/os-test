/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// test_setxid.c — M1.1 (unistd.md §2.A1/A2) verification for the
// setresuid/setresgid/setreuid/setregid syscalls + getgroups.
//
// Permission-ladder semantics (man setresuid(2)/setreuid(2)), process-wide
// (this kernel is not Linux per-thread; see unistd.md §4.3). The process boots
// as root (uid 0), so root-drop is irreversible within one process. Every case
// that drops root forks into a fresh child (which starts root, inherits nothing
// stale), runs the ladder steps, and exits 0/1; the parent stays root and
// asserts the child's exit status (mirrors test_setuid_saved.c).
//
// Cases:
//   1. setresuid root branch sets all three; climbing back to 0 then fails.
//   2. setresuid -1 leaves fields unchanged → euid-only drop keeps suid==0, so
//      seteuid(0) recovers root (the contrast that proves -1 is honoured).
//   3. setresuid non-root ladder: may only set each id to one of the current
//      real/effective/saved; a value none of those hold → -EPERM.
//   4. setresgid mirrors setresuid over gid/egid/sgid.
//   5. seteuid/setegid are setres*id(-1, x, -1) wrappers (suid preserved).
//   6. setreuid suid-preservation: changing euid away from the previous real
//      scrubs the saved-set to the new euid, so a later seteuid(0) fails.
//   7. setregid mirrors setreuid over gid/egid/sgid.
//   8. getgroups: getgroups(0, NULL) and getgroups(N, buf) both return 0 in the
//      M1.1 baseline (no supplementary groups).

// setresuid/setresgid/setreuid/setregid are GNU extensions in musl's
// <unistd.h> (#ifdef _GNU_SOURCE). The pre-migration repo <unistd.h> declared
// them unconditionally; musl gates them, so the test must opt in.
#define _GNU_SOURCE
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

// ---- 1. setresuid root branch sets real+effective+saved; climb-back fails
// ---- setresuid(1000,1000,1000) → all three 1000. setresuid(-1,0,-1) must fail
// (-EPERM): 0 is none of the current real/effective/saved (all 1000).
void test_setresuid_root_drop_then_eperm(void) {
  pid_t child = fork();
  if (child == 0) {
    FAIL_IF(setresuid(1000, 1000, 1000) != 0);
    FAIL_IF(getuid() != 1000 || geteuid() != 1000);
    if (setresuid(-1, 0, -1) != -1 || errno != EPERM)
      _exit(1);
    _exit(0);
  }
  int status = 0;
  TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
}

// ---- 2. setresuid -1 leaves uid/suid unchanged → root recoverable ----
// setresuid(-1, 1000, -1) drops only euid; uid and suid stay 0. seteuid(0) then
// succeeds (suid still 0). This is the guard that -1 is truly "unchanged": if
// the kernel had clobbered suid to 1000, seteuid(0) would fail.
void test_setresuid_minus1_keeps_suid(void) {
  pid_t child = fork();
  if (child == 0) {
    FAIL_IF(setresuid(-1, 1000, -1) != 0);
    FAIL_IF(getuid() != 0 || geteuid() != 1000);
    // suid still 0 → seteuid(0) recovers root.
    FAIL_IF(seteuid(0) != 0);
    FAIL_IF(geteuid() != 0);
    _exit(0);
  }
  int status = 0;
  TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
}

// ---- 3. setresuid non-root ladder: each id ∈ {real,effective,saved} ----
// After dropping to (1000,1000,1000): setresuid(-1,-1,2000) fails (2000 not
// held), but setresuid(-1,-1,1000) succeeds (1000 is held).
void test_setresuid_nonroot_ladder(void) {
  pid_t child = fork();
  if (child == 0) {
    FAIL_IF(setresuid(1000, 1000, 1000) != 0);
    if (setresuid(-1, -1, 2000) != -1 || errno != EPERM)
      _exit(1);
    FAIL_IF(setresuid(-1, -1, 1000) != 0);
    _exit(0);
  }
  int status = 0;
  TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
}

// ---- 4. setresgid mirrors setresuid over gid/egid/sgid ----
// Like the uid ladder, the unprivileged rules only bite once setgid privilege
// is gone. capable(CAP_SETGID) == euid==0 (no cap bitmap), so dropping gid
// alone leaves the privilege intact; drop euid too, then probe the ladder:
// setresgid(-1,0,-1) and (-1,-1,2000) fail (none held), (-1,-1,1000) succeeds.

void test_setresgid_ladder(void) {
  pid_t child = fork();
  if (child == 0) {
    FAIL_IF(setresgid(1000, 1000, 1000) != 0);
    FAIL_IF(getgid() != 1000 || getegid() != 1000);
    // Drop setgid privilege: gid alone doesn't, since CAP_SETGID tracks euid.
    FAIL_IF(setresuid(1000, 1000, 1000) != 0);
    if (setresgid(-1, 0, -1) != -1 || errno != EPERM)
      _exit(1);
    if (setresgid(-1, -1, 2000) != -1 || errno != EPERM)
      _exit(1);
    FAIL_IF(setresgid(-1, -1, 1000) != 0);
    _exit(0);
  }
  int status = 0;
  TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
}

// ---- 5. seteuid/setegid preserve the saved-set (setres*id(-1,x,-1)) ----
// seteuid(1000) drops euid only; seteuid(0) recovers because suid stayed 0.
// After a full setresuid(1000,1000,1000) drop, seteuid(0) must fail.
void test_seteuid_preserves_suid(void) {
  pid_t child = fork();
  if (child == 0) {
    FAIL_IF(seteuid(1000) != 0);
    FAIL_IF(geteuid() != 1000);
    FAIL_IF(seteuid(0) != 0); // recovered: suid still 0
    // Now a full drop, then seteuid(0) fails.
    FAIL_IF(setresuid(1000, 1000, 1000) != 0);
    if (seteuid(0) != -1 || errno != EPERM)
      _exit(1);
    _exit(0);
  }
  int status = 0;
  TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
}

// ---- 6. setreuid suid-preservation scrubs the saved-set ----
// setreuid(1000, 1000): ruid→1000, euid→1000, and since ruid was set the
// saved-set is scrubbed to the new euid (1000). All three now hold 1000, so
// seteuid(0) fails — root is gone from {real, effective, saved} entirely.
// (Using 1000/1000 rather than -1/1000 so the real-uid back-door isn't the
// thing that defeats seteuid(0); the scrubbed saved-set is.)
void test_setreuid_scrubs_suid(void) {
  pid_t child = fork();
  if (child == 0) {
    FAIL_IF(setreuid(1000, 1000) != 0);
    FAIL_IF(getuid() != 1000 || geteuid() != 1000);
    if (seteuid(0) != -1 || errno != EPERM)
      _exit(1);
    _exit(0);
  }
  int status = 0;
  TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
}

// ---- 6b. setreuid setting ruid also scrubs the saved-set ----
// setreuid(1000, -1): ruid→1000, euid stays 0. The rule fires because ruid was
// set, so suid becomes the new euid (0). euid is still 0, so seteuid(1000)
// drops it, and seteuid(0) recovers — proving suid was set to 0, not clobbered
// to 1000. (The interesting bit: ruid is now 1000 but suid==0, so a subsequent
// full setresuid that needs suid 0 still works.)
void test_setreuid_ruid_sets_suid_to_euid(void) {
  pid_t child = fork();
  if (child == 0) {
    FAIL_IF(setreuid(1000, -1) != 0);
    FAIL_IF(getuid() != 1000 || geteuid() != 0);
    // suid was set to the new euid (0); seteuid(1000) then seteuid(0) works.
    FAIL_IF(seteuid(1000) != 0);
    FAIL_IF(seteuid(0) != 0);
    _exit(0);
  }
  int status = 0;
  TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
}

// ---- 7. setregid mirrors setreuid over gid/egid/sgid ----
// setregid(1000, 1000): rgid→1000, egid→1000, saved-set scrubbed to 1000.
// All three gid slots hold 1000, so setegid(0) fails — 0 is held by none.
// (Drop euid too via setresuid so CAP_SETGID isn't still granting it; and
// rgid no longer 0 so there's no real-gid back-door — the scrub is what fails.)

void test_setregid_scrubs_sgid(void) {
  pid_t child = fork();
  if (child == 0) {
    FAIL_IF(setregid(1000, 1000) != 0);
    FAIL_IF(getgid() != 1000 || getegid() != 1000);
    FAIL_IF(setresuid(1000, 1000, 1000) != 0);
    if (setegid(0) != -1 || errno != EPERM)
      _exit(1);
    _exit(0);
  }
  int status = 0;
  TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
}

// ---- 8. getgroups baseline: 0 supplementary groups ----
// getgroups(0, NULL) → 0 (count only). getgroups(N, buf) with N>=0 → 0 (no
// groups written). The baseline is POSIX-legal: a process may have no
// supplementary groups (M1.1; group management lands later).
void test_getgroups_baseline_zero(void) {
  TEST_ASSERT_EQUAL_INT(0, getgroups(0, NULL));
  gid_t buf[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD, 0xBE, 0xEF};
  // No groups written; buffer must be untouched (we cannot assert the count
  // path wrote nothing without groups, but the return is 0).
  TEST_ASSERT_EQUAL_INT(0, getgroups(8, buf));
  (void)buf;
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_setresuid_root_drop_then_eperm);
  RUN_TEST(test_setresuid_minus1_keeps_suid);
  RUN_TEST(test_setresuid_nonroot_ladder);
  RUN_TEST(test_setresgid_ladder);
  RUN_TEST(test_seteuid_preserves_suid);
  RUN_TEST(test_setreuid_scrubs_suid);
  RUN_TEST(test_setreuid_ruid_sets_suid_to_euid);
  RUN_TEST(test_setregid_scrubs_sgid);
  RUN_TEST(test_getgroups_baseline_zero);
  return UNITY_END();
}

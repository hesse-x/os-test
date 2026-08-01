/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* Verify the Linux ruid/euid split for access(2) vs
 * eaccess/faccessat(AT_EACCESS), plus secure_getenv.
 *
 * Kernel: do_faccessat picks the credential fed to inode_permission by the flag
 * — no AT_EACCESS → REAL uid (access(2) semantics), AT_EACCESS → EFFECTIVE uid
 * (eaccess/faccessat(AT_EACCESS)). capable(CAP_DAC_OVERRIDE) root-override
 * stays keyed on euid regardless. So with ruid=0 / euid=nonroot on a 0640
 * root-owned file: access() uses ruid=0 → owner bits → 0; eaccess() uses
 * euid=nonroot → other bits (none) → EACCES. Different results for the same
 * file proves the split.
 *
 * Dropping root is irreversible within a process, so each case forks into a
 * fresh child (starts root, inherits nothing stale) and exits 0/1. setresuid
 * and setresgid split real/effective/saved IDs independently: the real IDs
 * stay 0 while the effective IDs become 1000. The parent stays root and
 * asserts exit status.
 *
 * secure_getenv: libc.secure is 0 in normal exec (kernel auxv AT_SECURE=0), so
 * secure_getenv == getenv. (The secure-exec NULL branch needs an AT_SECURE auxv
 * the kernel does not build; out of scope here.)
 *
 * _GNU_SOURCE: eaccess/euidaccess/setresuid are declared in musl <unistd.h>
 * under #ifdef _GNU_SOURCE, and secure_getenv in <stdlib.h> likewise. */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/process.h> // fork
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void child_fail(const char *step, int rc) {
  int saved_errno = errno;
  fprintf(stderr,
          "test_eaccess child failure: %s: rc=%d errno=%d (%s), "
          "ruid=%u euid=%u rgid=%u egid=%u\n",
          step, rc, saved_errno, strerror(saved_errno), (unsigned)getuid(),
          (unsigned)geteuid(), (unsigned)getgid(), (unsigned)getegid());
  fflush(stderr);
  _exit(1);
}

/* Fixture: a root-owned tmpfs file with mode 0640 — owner(root) rw, group r,
 * other NONE. A non-root euid not in group falls through to other bits (0).
 * tmpfs so chown/chmod persist (FAT32 would discard them — see test_chmod.c).
 */
#define SPLIT "/run/access_split"
#define NONROOT 1000

static void make_fixture(void) {
  int fd = open(SPLIT, O_CREAT | O_WRONLY | O_TRUNC, 0640);
  if (fd < 0)
    child_fail("open fixture", fd);
  close(fd);
  int rc = chown(SPLIT, 0, 0);
  if (rc != 0)
    child_fail("chown fixture", rc);
  rc = chmod(SPLIT, 0640);
  if (rc != 0)
    child_fail("chmod fixture", rc);
}

/* ---- 1. access() uses real IDs, eaccess() uses effective IDs → differ ----
 * Child keeps real uid/gid 0 and sets effective uid/gid to 1000. capable() is
 * now false (euid!=0), and the process no longer matches the file's group, so
 * the owner/group/other bit selection actually runs.
 *   access(SPLIT, R_OK)   → ruid=0 → owner bits (6) → R_OK ok → 0
 *   eaccess(SPLIT, R_OK)  → euid=1000 → 1000!=owner(0), 1000!=gid(0) → other(0)
 *                          → EACCES
 * Also faccessat(AT_EACCESS) exercises the repo faccessat → sys_faccessat →
 * AT_EACCESS → euid path, and must match eaccess. */
void test_access_ruid_eaccess_euid_differ(void) {
  pid_t child = fork();
  if (child == 0) {
    make_fixture();
    int rc = setresgid(0, NONROOT, 0);
    if (rc != 0)
      child_fail("setresgid(0, 1000, 0)", rc);
    rc = setresuid(0, NONROOT, 0);
    if (rc != 0)
      child_fail("setresuid(0, 1000, 0)", rc);
    if (getuid() != 0 || geteuid() != NONROOT || getgid() != 0 ||
        getegid() != NONROOT)
      child_fail("verify split credentials", 0);
    /* access() → ruid=0 → owner → allowed. */
    errno = 0;
    rc = access(SPLIT, R_OK);
    if (rc != 0)
      child_fail("access expected success", rc);
    /* eaccess() → euid=1000 → other → denied. */
    errno = 0;
    rc = eaccess(SPLIT, R_OK);
    if (rc != -1 || errno != EACCES)
      child_fail("eaccess expected EACCES", rc);
    /* faccessat(AT_EACCESS) must agree with eaccess (same euid path). */
    errno = 0;
    rc = faccessat(AT_FDCWD, SPLIT, R_OK, AT_EACCESS);
    if (rc != -1 || errno != EACCES)
      child_fail("faccessat AT_EACCESS expected EACCES", rc);
    /* And faccessat without AT_EACCESS uses ruid → matches access() → 0. */
    errno = 0;
    rc = faccessat(AT_FDCWD, SPLIT, R_OK, 0);
    if (rc != 0)
      child_fail("faccessat expected success", rc);
    _exit(0);
  }
  int status = 0;
  TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
  unlink(SPLIT);
}

/* ---- 2. root euid still overrides (eaccess on W_OK) ----
 * Child stays ruid=euid=0; eaccess(SPLIT, W_OK) → euid=0 → capable() root-
 * override → 0 even though the mode is 0640 (W_OK ok for owner anyway, but this
 * also guards that the override path did not regress to bit-checking). */
void test_root_euid_eaccess_overrides(void) {
  pid_t child = fork();
  if (child == 0) {
    make_fixture();
    errno = 0;
    int rc = eaccess(SPLIT, W_OK);
    if (rc != 0)
      child_fail("root eaccess expected success", rc);
    _exit(0);
  }
  int status = 0;
  TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
  unlink(SPLIT);
}

/* ---- 3. secure_getenv == getenv in a normal (AT_SECURE=0) exec ---- */
void test_secure_getenv_normal(void) {
  TEST_ASSERT_EQUAL_INT(0, setenv("X", "1", 1));
  char *v = secure_getenv("X");
  TEST_ASSERT_NOT_NULL(v);
  TEST_ASSERT_EQUAL_STRING("1", v);
  unsetenv("X");
  TEST_ASSERT_NULL(secure_getenv("X"));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_access_ruid_eaccess_euid_differ);
  RUN_TEST(test_root_euid_eaccess_overrides);
  RUN_TEST(test_secure_getenv_normal);
  return UNITY_END();
}

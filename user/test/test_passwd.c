/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// getpwnam/getpwuid/getgrnam/getgrgid + _r variants — musl src/passwd
// (musl_passwd_objs, passwd_worklist). The kernel does not pre-create /etc
// (vfs.c only makes /dev /sys /proc /run), and ships no /etc/passwd or
// /etc/group, so the suite writes them itself before lookup — mirroring the
// tmp-dir-not-precreated pattern in test_tmpfile. Until the files exist every
// lookup returns NULL + errno=ENOENT (correct "not found"); with them present
// musl parses the colon fields and fills struct passwd/group.

#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

// mkdir /etc if missing (ignore EEXIST), then write minimal passwd + group.
// Idempotent: re-created each run so stale content never leaks across runs.
static void ensure_passwd_files(void) {
  if (mkdir("/etc", 0755) != 0 && errno != EEXIST)
    TEST_FAIL_MESSAGE("mkdir(/etc) failed");

  FILE *f = fopen("/etc/passwd", "w");
  TEST_ASSERT_NOT_NULL(f);
  // name:passwd:uid:gid:gecos:dir:shell
  TEST_ASSERT_TRUE(fputs("root:x:0:0:root:/root:/bin/sh\n", f) != EOF);
  TEST_ASSERT_TRUE(
      fputs("alice:x:1000:1000:Alice User:/home/alice:/bin/sh\n", f) != EOF);
  fclose(f);

  f = fopen("/etc/group", "w");
  TEST_ASSERT_NOT_NULL(f);
  // name:passwd:gid:members
  TEST_ASSERT_TRUE(fputs("root:x:0:\n", f) != EOF);
  TEST_ASSERT_TRUE(fputs("users:x:1000:alice\n", f) != EOF);
  fclose(f);
}

// 1. getpwnam("root"): classic non-reentrant lookup, fields parsed.
void test_getpwnam_root(void) {
  ensure_passwd_files();
  errno = 0;
  struct passwd *pw = getpwnam("root");
  TEST_ASSERT_NOT_NULL(pw);
  TEST_ASSERT_EQUAL_STRING("root", pw->pw_name);
  TEST_ASSERT_EQUAL_STRING("x", pw->pw_passwd);
  TEST_ASSERT_EQUAL_INT(0, pw->pw_uid);
  TEST_ASSERT_EQUAL_INT(0, pw->pw_gid);
  TEST_ASSERT_EQUAL_STRING("/root", pw->pw_dir);
  TEST_ASSERT_EQUAL_STRING("/bin/sh", pw->pw_shell);
}

// 2. getpwuid(1000): lookup by uid.
void test_getpwuid_alice(void) {
  ensure_passwd_files();
  struct passwd *pw = getpwuid(1000);
  TEST_ASSERT_NOT_NULL(pw);
  TEST_ASSERT_EQUAL_STRING("alice", pw->pw_name);
  TEST_ASSERT_EQUAL_INT(1000, pw->pw_uid);
  TEST_ASSERT_EQUAL_STRING("Alice User", pw->pw_gecos);
}

// 3. getpwnam on a missing user: NULL + errno == 0 (musl semantics: not-found
//    is not an error).
void test_getpwnam_missing(void) {
  ensure_passwd_files();
  errno = 0;
  struct passwd *pw = getpwnam("nobody");
  TEST_ASSERT_NULL(pw);
  TEST_ASSERT_EQUAL_INT(0, errno);
}

// 4. getpwnam_r: reentrant variant, success path.
void test_getpwnam_r_ok(void) {
  ensure_passwd_files();
  struct passwd pw;
  struct passwd *res;
  char buf[256];
  int rv = getpwnam_r("root", &pw, buf, sizeof(buf), &res);
  TEST_ASSERT_EQUAL_INT(0, rv);
  TEST_ASSERT_NOT_NULL(res);
  TEST_ASSERT_EQUAL_STRING("root", res->pw_name);
  TEST_ASSERT_EQUAL_INT(0, res->pw_uid);
}

// 5. getpwnam_r with a too-small buffer: ERANGE + res NULL.
void test_getpwnam_r_erange(void) {
  ensure_passwd_files();
  struct passwd pw;
  struct passwd *res;
  char buf[1]; // deliberately tiny
  int rv = getpwnam_r("alice", &pw, buf, sizeof(buf), &res);
  TEST_ASSERT_EQUAL_INT(ERANGE, rv);
  TEST_ASSERT_NULL(res);
}

// 6. getgrnam("users"): group lookup with a member list.
void test_getgrnam_users(void) {
  ensure_passwd_files();
  struct group *gr = getgrnam("users");
  TEST_ASSERT_NOT_NULL(gr);
  TEST_ASSERT_EQUAL_STRING("users", gr->gr_name);
  TEST_ASSERT_EQUAL_INT(1000, gr->gr_gid);
  TEST_ASSERT_NOT_NULL(gr->gr_mem);
  TEST_ASSERT_EQUAL_STRING("alice", gr->gr_mem[0]);
  TEST_ASSERT_NULL(gr->gr_mem[1]);
}

// 7. getgrgid(0): root group, empty member list.
void test_getgrgid_root(void) {
  ensure_passwd_files();
  struct group *gr = getgrgid(0);
  TEST_ASSERT_NOT_NULL(gr);
  TEST_ASSERT_EQUAL_STRING("root", gr->gr_name);
  TEST_ASSERT_EQUAL_INT(0, gr->gr_gid);
  TEST_ASSERT_NOT_NULL(gr->gr_mem);
  TEST_ASSERT_NULL(gr->gr_mem[0]);
}

// 8. getpwent: sequential enumeration over /etc/passwd, reset by setpwent.
void test_getpwent_iteration(void) {
  ensure_passwd_files();
  setpwent();
  struct passwd *pw = getpwent();
  TEST_ASSERT_NOT_NULL(pw);
  TEST_ASSERT_EQUAL_STRING("root", pw->pw_name);
  pw = getpwent();
  TEST_ASSERT_NOT_NULL(pw);
  TEST_ASSERT_EQUAL_STRING("alice", pw->pw_name);
  pw = getpwent();
  TEST_ASSERT_NULL(pw); // end of file
  endpwent();
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();
  RUN_TEST(test_getpwnam_root);
  RUN_TEST(test_getpwuid_alice);
  RUN_TEST(test_getpwnam_missing);
  RUN_TEST(test_getpwnam_r_ok);
  RUN_TEST(test_getpwnam_r_erange);
  RUN_TEST(test_getgrnam_users);
  RUN_TEST(test_getgrgid_root);
  RUN_TEST(test_getpwent_iteration);
  return UNITY_END();
}

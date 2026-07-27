/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_permission.c — permission subsystem (permission.md §5.4).
 *
 * Verifies the post-baseline permission semantics:
 *   1. Identity baseline: getuid()==getgid()==1000 (DEFAULT_UID/GID), and
 *      geteuid()==getuid() (no setuid escalation happened at exec).
 *   2. open(O_CREAT, mode) applies umask (existing S08 behaviour, regression
 *      guard).
 *   3. chmod(path, 0600) writes the inode mode; stat reads it back.
 *   4. chown(path, 1000, 1000) writes owner/group; stat reads them back.
 *   5. chown(path, -1, 1001) leaves uid unchanged (POSIX "-1 = unchanged").
 *   6. fchmod(fd, 0755) writes via the fd path; fstat reads it back.
 *   7. fchmodat(AT_FDCWD, path, 0644, 0) writes via the *at path.
 *   8. open(O_WRONLY) succeeds — inode_permission placeholder allows all.
 *   9. mkdir(dir, 0755) succeeds — placeholder does not block creation.
 *
 * All chmod/chown state lives in the in-memory inode cache (FAT32 has no
 * on-disk permission bits), so the assertions hold within one boot session. */
#include "unity.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* FAT32-root fixture namespace (mirrors test_stat_real's /stat_real_fat). */
#define FAT "/permission_test_fat"

void setUp(void) {
  /* Create the fixture parent dir once (ok if it already exists). open/mkdir
   * only create the final component, so the parent (/permission_test_fat)
   * must pre-exist. */
  mkdir(FAT, 0755);
}
void tearDown(void) {}

static void cleanup(void) {
  unlink(FAT "/u");
  rmdir(FAT "/d");
}

/* Identity baseline: single-user non-privileged model, uid=gid=1000. */
void test_identity_baseline(void) {
  TEST_ASSERT_EQUAL_INT(1000, (int)getuid());
  TEST_ASSERT_EQUAL_INT(1000, (int)getgid());
  /* No setuid escalation at exec: euid==uid, egid==gid. */
  TEST_ASSERT_EQUAL_INT((int)getuid(), (int)geteuid());
  TEST_ASSERT_EQUAL_INT((int)getgid(), (int)getegid());
}

/* umask applied to open(O_CREAT): mode & ~umask lands in st_mode. Guards
 * against the S08 creation path regressing when the permission call sites are
 * inserted. */
void test_open_umask_applied(void) {
  cleanup();
  mode_t old = umask(0022);
  int fd = open(FAT "/u", O_CREAT | O_RDWR | O_TRUNC, 0666);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);

  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, fstat(fd, &st));
  TEST_ASSERT_EQUAL_INT(0666 & ~0022, st.st_mode & 0777);
  close(fd);
  umask(old);
  unlink(FAT "/u");
}

/* chmod(path, 0600) writes the inode mode; stat reads it back. */
void test_chmod_writes_mode(void) {
  cleanup();
  int fd = open(FAT "/u", O_CREAT | O_RDWR | O_TRUNC, 0666);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);

  TEST_ASSERT_EQUAL_INT(0, chmod(FAT "/u", 0600));

  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(FAT "/u", &st));
  TEST_ASSERT_TRUE(S_ISREG(st.st_mode));
  TEST_ASSERT_EQUAL_INT(0600, st.st_mode & 0777);
  unlink(FAT "/u");
}

/* chown(path, 1000, 1000) writes owner/group; stat reads them back. */
void test_chown_writes_owner(void) {
  cleanup();
  int fd = open(FAT "/u", O_CREAT | O_RDWR | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);

  TEST_ASSERT_EQUAL_INT(0, chown(FAT "/u", 1000, 1000));

  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(FAT "/u", &st));
  TEST_ASSERT_EQUAL_INT(1000, (int)st.st_uid);
  TEST_ASSERT_EQUAL_INT(1000, (int)st.st_gid);
  unlink(FAT "/u");
}

/* chown(path, -1, 1001) leaves uid unchanged (POSIX "-1 = unchanged"). The
 * (uid_t)-1 sentinel must survive the uint32_t narrowing into the kernel cred
 * fields. */
void test_chown_minus1_unchanged(void) {
  cleanup();
  int fd = open(FAT "/u", O_CREAT | O_RDWR | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);

  /* Establish a known owner first. */
  TEST_ASSERT_EQUAL_INT(0, chown(FAT "/u", 1000, 1000));
  /* Change only gid; uid stays 1000. */
  TEST_ASSERT_EQUAL_INT(0, chown(FAT "/u", (uid_t)-1, 1001));

  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(FAT "/u", &st));
  TEST_ASSERT_EQUAL_INT(1000, (int)st.st_uid);
  TEST_ASSERT_EQUAL_INT(1001, (int)st.st_gid);
  unlink(FAT "/u");
}

/* Recreate-at-same-slot must NOT inherit the dead file's metadata. FAT32 inos
 * are position-based (dir_cluster * EPC + dir_idx), so after unlink + creat at
 * the same path the new file lands in the same dir slot → same ino. The kernel
 * marks the deleted inode nlink=0 and the cache-hit scan skips it, so the new
 * file gets a fresh inode with default mode 0644 — not the 0600 the dead file
 * held. This is the single most important guard for the positional-ino hazard
 * the cache-pinning fix introduces. */
void test_recreate_same_slot_resets_mode(void) {
  cleanup();
  int fd = open(FAT "/u", O_CREAT | O_RDWR | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);

  TEST_ASSERT_EQUAL_INT(0, chmod(FAT "/u", 0600));
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(FAT "/u", &st));
  TEST_ASSERT_EQUAL_INT(0600, st.st_mode & 0777);

  TEST_ASSERT_EQUAL_INT(0, unlink(FAT "/u"));

  /* Recreate at the same path → same positional dir slot → same ino. The new
   * inode must be a fresh one with default 0644, proving the nlink==0 skip
   * prevented it from reusing the dead 0600 inode. */
  fd = open(FAT "/u", O_CREAT | O_RDWR | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);

  TEST_ASSERT_EQUAL_INT(0, stat(FAT "/u", &st));
  TEST_ASSERT_EQUAL_INT(0644, st.st_mode & 0777);
  unlink(FAT "/u");
}

/* fchmod(fd, 0755) writes via the fd path; fstat reads it back. */
void test_fchmod_fd_path(void) {
  cleanup();
  int fd = open(FAT "/u", O_CREAT | O_RDWR | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);

  TEST_ASSERT_EQUAL_INT(0, fchmod(fd, 0755));

  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, fstat(fd, &st));
  TEST_ASSERT_EQUAL_INT(0755, st.st_mode & 0777);
  close(fd);
  unlink(FAT "/u");
}

/* fchmodat(AT_FDCWD, path, 0644, 0) writes via the *at path. */
void test_fchmodat_at_path(void) {
  cleanup();
  int fd = open(FAT "/u", O_CREAT | O_RDWR | O_TRUNC, 0600);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);

  TEST_ASSERT_EQUAL_INT(0, fchmodat(AT_FDCWD, FAT "/u", 0644, 0));

  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(FAT "/u", &st));
  TEST_ASSERT_EQUAL_INT(0644, st.st_mode & 0777);
  unlink(FAT "/u");
}

/* open(O_WRONLY) succeeds — inode_permission placeholder allows all (no
 * regression vs. the pre-permission open behaviour). */
void test_open_writable_succeeds(void) {
  cleanup();
  int fd = open(FAT "/u", O_CREAT | O_RDWR | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);

  int wfd = open(FAT "/u", O_WRONLY);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, wfd);
  close(wfd);
  unlink(FAT "/u");
}

/* mkdir(dir, 0755) succeeds — placeholder does not block directory creation. */
void test_mkdir_succeeds(void) {
  cleanup();
  TEST_ASSERT_EQUAL_INT(0, mkdir(FAT "/d", 0755));

  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(FAT "/d", &st));
  TEST_ASSERT_TRUE(S_ISDIR(st.st_mode));
  rmdir(FAT "/d");
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_identity_baseline);
  RUN_TEST(test_open_umask_applied);
  RUN_TEST(test_chmod_writes_mode);
  RUN_TEST(test_chown_writes_owner);
  RUN_TEST(test_chown_minus1_unchanged);
  RUN_TEST(test_recreate_same_slot_resets_mode);
  RUN_TEST(test_fchmod_fd_path);
  RUN_TEST(test_fchmodat_at_path);
  RUN_TEST(test_open_writable_succeeds);
  RUN_TEST(test_mkdir_succeeds);
  return UNITY_END();
}

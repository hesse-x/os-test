/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// test_chmod.c — verify chmod/fchmod/fchmodat/chown/fchown/fchownat(2) (phase 1
// real impl + phase 0 capable() gating). Mirrors test_link_utimensat.c: Unity
// freestanding, FAT32 (/ prefix) + tmpfs (/run prefix) dual fixtures.
//
// Persisted in-memory only (like utimensat Q5). setuid-bit clearing rules
// (apply_chmod/apply_chown):
//   - Unprivileged (no CAP_FSETID) chmod/chown clears S_ISUID/S_ISGID —
//   prevents
//     forging a setuid-root escalation.
//   - Root (with CAP_FSETID) chmod/chown keeps S_ISUID — prerequisite for sudo.
// open(O_CREAT) only persists the 0777 bits (& 0777); S_ISUID must be set via
// chmod, so setuid cases chmod 04755 first. Non-root cases fork a child that
// setuid(1000) + _exit's a code the parent waitpid-asserts (cf.
// test_kill_perm.c:106).
//
// FAT32 vs tmpfs (like test_link_utimensat): fat32 inodes have no fs-internal
// strong ref, so after chmod/chown releases the inode it may be reclaimed; a
// later stat re-creates it via lookup → mode/uid/gid reset to inode_create
// defaults (0100644 / uid=0). So path-set + path-verify cases (set→stat both
// re-resolve via path) use tmpfs (children hold inode refs, no reclaim, exact
// round-trip). fd-path cases (open holds the inode ref; fchmod/fstat share the
// fd) use FAT32 as a cross-check, proving fd and path converge.
#include "unity.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/process.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define FAT "/chmod_fat"
#define TFS "/run/chmod_tfs"

void setUp(void) {
  // Fixture parent dirs (idempotent: mkdir only creates the last segment).
  mkdir(FAT, 0755);
  mkdir(TFS, 0755);
}
void tearDown(void) {}

// cleanup: remove per-test files, keep the fixture parent dirs.
static void cleanup(void) {
  unlink(FAT "/f");
  unlink(TFS "/f");
}

// Create an empty 0644 file for chmod/chown. Returns fd (>=0) or -1.
static int make_file(const char *path) {
  int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd >= 0)
    close(fd);
  return fd;
}

// 1. root chmod → stat asserts mode (proves a real inode write, not a stub).
// tmpfs: children hold the inode ref; after chmod releases it, stat re-resolves
// and hits the cache → exact mode round-trip.
void test_chmod_root_basic(void) {
  cleanup();
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, make_file(TFS "/f"));
  TEST_ASSERT_EQUAL_INT(0, chmod(TFS "/f", 0600));
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(TFS "/f", &st));
  TEST_ASSERT_EQUAL_INT(0600, st.st_mode & 07777);
}

// 2. chmod preserves the type bits: S_ISREG/S_ISDIR still hold.
void test_chmod_preserves_type(void) {
  cleanup();
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, make_file(TFS "/f"));
  TEST_ASSERT_EQUAL_INT(0, chmod(TFS "/f", 0600));
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(TFS "/f", &st));
  TEST_ASSERT_TRUE(S_ISREG(st.st_mode));
  // Directory type bit preserved: tmpfs fixture dir still S_ISDIR after chmod.
  TEST_ASSERT_EQUAL_INT(0, chmod(TFS, 0700));
  TEST_ASSERT_EQUAL_INT(0, stat(TFS, &st));
  TEST_ASSERT_TRUE(S_ISDIR(st.st_mode));
}

// 3. Non-root owner chmod of its own 04755 file → S_ISUID cleared.
// The non-root owner has chmod permission (euid==ip->uid) but no CAP_FSETID, so
// apply_chmod clears the setuid bit. The bit must be set by root chmod 04755
// (CAP_FSETID keeps it); the owner is chown'd to 1000 first.
void test_chmod_owner_clears_setuid(void) {
  cleanup();
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, make_file(TFS "/f"));
  // root chown to 1000 (this round chown is root-only; see
  // test_chown_root_basic).
  TEST_ASSERT_EQUAL_INT(0, chown(TFS "/f", 1000, 1000));
  // root chmod 04755 sets S_ISUID (CAP_FSETID keeps it).
  TEST_ASSERT_EQUAL_INT(0, chmod(TFS "/f", 04755));
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(TFS "/f", &st));
  TEST_ASSERT_TRUE(st.st_mode & S_ISUID);

  pid_t child = fork();
  if (child == 0) {
    setuid(1000); // drop root: uid==euid==1000, owner of /run/chmod_tfs/f
    // owner chmod 0644 (unprivileged): apply_chmod clears S_ISUID.
    int r = chmod(TFS "/f", 0644);
    if (r != 0)
      _exit(2);
    struct stat s2;
    if (stat(TFS "/f", &s2) != 0)
      _exit(3);
    _exit((s2.st_mode & S_ISUID) ? 1 : 0); // 0 = cleared as expected
  }
  int status = 0;
  TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
}

// 4. Non-root chmod of a root-owned file → EPERM.
// root-owned tmpfs file: non-root euid != ip->uid (0) and no CAP_FOWNER → EPERM
// (matches Linux/POSIX chmod(2): non-owner without CAP_FOWNER returns EPERM,
// not EACCES).
void test_chmod_non_root_eperm(void) {
  cleanup();
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, make_file(TFS "/f"));
  pid_t child = fork();
  if (child == 0) {
    setuid(1000);
    int r = chmod(TFS "/f", 0600);
    _exit(r == -1 && errno == EPERM ? 0 : 1); // 0 = got EPERM as expected
  }
  int status = 0;
  TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
}

// 5. root chmod 04755 → S_ISUID kept (CAP_FSETID exemption).
void test_chmod_root_setuid_kept(void) {
  cleanup();
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, make_file(TFS "/f"));
  TEST_ASSERT_EQUAL_INT(0, chmod(TFS "/f", 04755));
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(TFS "/f", &st));
  TEST_ASSERT_TRUE(st.st_mode & S_ISUID);
}

// 6. fchmod(fd) → fstat(fd) asserts 0600.
// FAT32 cross-check: open holds the inode ref; fchmod and fstat share the fd
// (no reclaim).
void test_fchmod_fd_path(void) {
  cleanup();
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, make_file(FAT "/f"));
  int fd = open(FAT "/f", O_WRONLY);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  TEST_ASSERT_EQUAL_INT(0, fchmod(fd, 0600));
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, fstat(fd, &st));
  TEST_ASSERT_EQUAL_INT(0600, st.st_mode & 07777);
  close(fd);
}

// 7. fchmodat(fd,"",0600,AT_EMPTY_PATH) → fstat(fd) asserts 0600.
void test_fchmodat_at_empty_path(void) {
  cleanup();
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, make_file(FAT "/f"));
  int fd = open(FAT "/f", O_WRONLY);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  TEST_ASSERT_EQUAL_INT(0, fchmodat(fd, "", 0600, AT_EMPTY_PATH));
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, fstat(fd, &st));
  TEST_ASSERT_EQUAL_INT(0600, st.st_mode & 07777);
  close(fd);
}

// 8. fchmodat dirfd relative-path resolution.
// tmpfs: after fchmodat(dirfd,"f") releases, stat(path) re-resolves and hits
// the cache (children hold the ref).
void test_fchmodat_dirfd_relative(void) {
  cleanup();
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, make_file(TFS "/f"));
  int dfd = open(TFS, O_RDONLY | O_DIRECTORY);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, dfd);
  TEST_ASSERT_EQUAL_INT(0, fchmodat(dfd, "f", 0600, 0));
  close(dfd);
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(TFS "/f", &st));
  TEST_ASSERT_EQUAL_INT(0600, st.st_mode & 07777);
}

// 9. root chown(1000,1000) → stat asserts uid/gid.
void test_chown_root_basic(void) {
  cleanup();
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, make_file(TFS "/f"));
  TEST_ASSERT_EQUAL_INT(0, chown(TFS "/f", 1000, 1000));
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(TFS "/f", &st));
  TEST_ASSERT_EQUAL_INT(1000, (int)st.st_uid);
  TEST_ASSERT_EQUAL_INT(1000, (int)st.st_gid);
}

// 10. chown(-1,-1) leaves uid/gid unchanged (regression guard).
void test_chown_minus_one_unchanged(void) {
  cleanup();
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, make_file(TFS "/f"));
  TEST_ASSERT_EQUAL_INT(0, chown(TFS "/f", 1000, 1000));
  // (uid_t)-1 / (gid_t)-1 = leave that field unchanged (POSIX).
  TEST_ASSERT_EQUAL_INT(0, chown(TFS "/f", (uid_t)-1, (gid_t)-1));
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(TFS "/f", &st));
  TEST_ASSERT_EQUAL_INT(1000, (int)st.st_uid);
  TEST_ASSERT_EQUAL_INT(1000, (int)st.st_gid);
}

// 11. Non-root chown → EPERM (this round simplifies to root-only).
// EPERM comes from the CAP_CHOWN gate (before any path/fs), so FAT32 is fine.
void test_chown_non_root_eperm(void) {
  cleanup();
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, make_file(FAT "/f"));
  pid_t child = fork();
  if (child == 0) {
    setuid(1000);
    int r = chown(FAT "/f", 1000, 1000);
    _exit(r == -1 && errno == EPERM ? 0 : 1); // 0 = got EPERM as expected
  }
  int status = 0;
  TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
}

// 12. root chown keeps S_ISUID (has CAP_FSETID).
// This round chown is root-only; non-root chown gets EPERM and never reaches
// the clear path. root chown is CAP_FSETID-exempt and keeps S_ISUID (matches
// apply_chown: clears only when !capable(CAP_FSETID)). Non-root chown clearing
// the setuid bit requires the "owner changes group to its own group" rule to
// be opened up; tracked in todo.
void test_chown_root_keeps_setuid(void) {
  cleanup();
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, make_file(TFS "/f"));
  TEST_ASSERT_EQUAL_INT(0, chmod(TFS "/f", 04755));
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(TFS "/f", &st));
  TEST_ASSERT_TRUE(st.st_mode & S_ISUID);
  TEST_ASSERT_EQUAL_INT(0, chown(TFS "/f", 1000, 1000));
  TEST_ASSERT_EQUAL_INT(0, stat(TFS "/f", &st));
  TEST_ASSERT_TRUE(st.st_mode & S_ISUID); // root chown keeps S_ISUID
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_chmod_root_basic);
  RUN_TEST(test_chmod_preserves_type);
  RUN_TEST(test_chmod_owner_clears_setuid);
  RUN_TEST(test_chmod_non_root_eperm);
  RUN_TEST(test_chmod_root_setuid_kept);
  RUN_TEST(test_fchmod_fd_path);
  RUN_TEST(test_fchmodat_at_empty_path);
  RUN_TEST(test_fchmodat_dirfd_relative);
  RUN_TEST(test_chown_root_basic);
  RUN_TEST(test_chown_minus_one_unchanged);
  RUN_TEST(test_chown_non_root_eperm);
  RUN_TEST(test_chown_root_keeps_setuid);
  return UNITY_END();
}

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// BSD flock(2) whole-file advisory locks.
//
// Pins the per-inode conflict / release semantics:
//   - LOCK_EX on fd1 vs independent fd2 LOCK_EX|LOCK_NB → EWOULDBLOCK.
//   - LOCK_UN releases; SH|SH compatible; EX conflicts with SH.
//   - dup()'d fds share one description → no self-conflict (upgrade/downgrade
//     within a description never blocks).
//   - Closing the last fd of a description releases its BSD flock.
//   - BSD flock and POSIX record locks are distinct universes (no conflict).
//   - Cross-process: parent's LOCK_EX blocks child's independent
// LOCK_EX|LOCK_NB.

#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/file.h>
#include <sys/process.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unity.h>

#include <xos/errno.h>

void setUp(void) {}
void tearDown(void) {}

static const char *g_path = "/local/flock_test.txt";

static int fresh_fd(int flags) {
  int fd = open(g_path, flags);
  TEST_ASSERT_TRUE(fd >= 0);
  return fd;
}

// LOCK_EX on fd1, independent fd2 LOCK_EX|LOCK_NB → EWOULDBLOCK; LOCK_UN
// releases; fd2 then succeeds.
void test_flock_ex_conflict_and_release(void) {
  int fd1 = fresh_fd(O_RDWR | O_CREAT);
  int fd2 = fresh_fd(O_RDWR);

  TEST_ASSERT_EQUAL_INT(0, flock(fd1, LOCK_EX));
  TEST_ASSERT_EQUAL_INT(-1, flock(fd2, LOCK_EX | LOCK_NB));
  TEST_ASSERT_EQUAL_INT(EWOULDBLOCK, errno);
  TEST_ASSERT_EQUAL_INT(0, flock(fd1, LOCK_UN));
  TEST_ASSERT_EQUAL_INT(0, flock(fd2, LOCK_EX | LOCK_NB));

  flock(fd2, LOCK_UN);
  close(fd1);
  close(fd2);
}

// SH|SH compatible; a third fd's LOCK_EX|LOCK_NB conflicts.
void test_flock_sh_shared_ex_conflicts(void) {
  int fd1 = fresh_fd(O_RDWR | O_CREAT);
  int fd2 = fresh_fd(O_RDWR);
  int fd3 = fresh_fd(O_RDWR);

  TEST_ASSERT_EQUAL_INT(0, flock(fd1, LOCK_SH));
  TEST_ASSERT_EQUAL_INT(0, flock(fd2, LOCK_SH | LOCK_NB));
  TEST_ASSERT_EQUAL_INT(-1, flock(fd3, LOCK_EX | LOCK_NB));
  TEST_ASSERT_EQUAL_INT(EWOULDBLOCK, errno);

  flock(fd1, LOCK_UN);
  flock(fd2, LOCK_UN);
  flock(fd3, LOCK_UN);
  close(fd1);
  close(fd2);
  close(fd3);
}

// dup()'d fds share one open file description → EX on both is OK.
void test_flock_dup_shares(void) {
  int fd1 = fresh_fd(O_RDWR | O_CREAT);
  int fd2 = dup(fd1);
  TEST_ASSERT_TRUE(fd2 >= 0);

  TEST_ASSERT_EQUAL_INT(0, flock(fd1, LOCK_EX));
  TEST_ASSERT_EQUAL_INT(0, flock(fd2, LOCK_EX)); // shared description

  flock(fd1, LOCK_UN);
  close(fd1);
  close(fd2);
}

// Upgrade (SH→EX) and downgrade (EX→SH) within one description never block.
void test_flock_upgrade_downgrade(void) {
  int fd = fresh_fd(O_RDWR | O_CREAT);

  TEST_ASSERT_EQUAL_INT(0, flock(fd, LOCK_SH));
  TEST_ASSERT_EQUAL_INT(0, flock(fd, LOCK_EX)); // upgrade
  TEST_ASSERT_EQUAL_INT(0, flock(fd, LOCK_SH)); // downgrade

  flock(fd, LOCK_UN);
  close(fd);
}

// Closing the last fd of a description releases its BSD flock (file_put path):
// a fresh open can then lock without conflict.
void test_flock_close_releases(void) {
  int fd1 = fresh_fd(O_RDWR | O_CREAT);
  TEST_ASSERT_EQUAL_INT(0, flock(fd1, LOCK_EX));
  close(fd1); // last fd → BSD lock released

  int fd2 = fresh_fd(O_RDWR);
  TEST_ASSERT_EQUAL_INT(0, flock(fd2, LOCK_EX | LOCK_NB)); // no lingering

  flock(fd2, LOCK_UN);
  close(fd2);
}

// BSD flock and POSIX record locks are distinct universes: a POSIX write lock
// does not block an overlapping BSD flock on a different description.
void test_flock_vs_posix_no_conflict(void) {
  int fd1 = fresh_fd(O_RDWR | O_CREAT);
  int fd2 = fresh_fd(O_RDWR);

  struct flock pl = {0};
  pl.l_type = F_WRLCK;
  pl.l_whence = SEEK_SET;
  pl.l_start = 0;
  pl.l_len = 16;
  TEST_ASSERT_EQUAL_INT(0, fcntl(fd1, F_SETLK, &pl));
  TEST_ASSERT_EQUAL_INT(0, flock(fd2, LOCK_EX | LOCK_NB)); // cross-universe

  flock(fd2, LOCK_UN);
  pl.l_type = F_UNLCK;
  fcntl(fd1, F_SETLK, &pl);
  close(fd1);
  close(fd2);
}

// Cross-process: parent holds LOCK_EX, child's independent open gets
// EWOULDBLOCK with LOCK_NB.
void test_flock_cross_process_conflict(void) {
  int fd = fresh_fd(O_RDWR | O_CREAT);
  write(fd, "x", 1);
  TEST_ASSERT_EQUAL_INT(0, flock(fd, LOCK_EX));

  pid_t pid = fork();
  TEST_ASSERT_TRUE(pid >= 0);
  if (pid == 0) {
    int cfd = open(g_path, O_RDWR);
    if (cfd < 0)
      _exit(100);
    int r = flock(cfd, LOCK_EX | LOCK_NB);
    if (r == -1 && errno == EWOULDBLOCK)
      _exit(0);
    _exit(1);
  }
  int status = 0;
  pid_t w = waitpid(pid, &status, 0);
  TEST_ASSERT_EQUAL_INT(pid, w);
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));

  flock(fd, LOCK_UN);
  close(fd);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_flock_ex_conflict_and_release);
  RUN_TEST(test_flock_sh_shared_ex_conflicts);
  RUN_TEST(test_flock_dup_shares);
  RUN_TEST(test_flock_upgrade_downgrade);
  RUN_TEST(test_flock_close_releases);
  RUN_TEST(test_flock_vs_posix_no_conflict);
  RUN_TEST(test_flock_cross_process_conflict);
  return UNITY_END();
}

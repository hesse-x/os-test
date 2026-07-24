/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* OFD (open file description) record locks — F_OFD_GETLK/SETLK/SETLKW.
 *
 * These tests pin the behavior that distinguishes OFD locks from POSIX
 * (per-process) locks:
 *   - Two independent open()s of the same file by the SAME process conflict
 *     (OFD is per-description), whereas POSIX F_SETLK on the same two fds
 *     does not (per-process, same pid).
 *   - dup()'d fds share one description → no conflict between them.
 *   - F_OFD_GETLK reports the conflicting holder's l_pid.
 *   - Closing the last fd of a description releases that description's OFD
 *     locks (a fresh open then locks cleanly).
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/process.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unity.h>

#include <xos/errno.h>

void setUp(void) {}
void tearDown(void) {}

static const char *g_path = "/local/fcntl_ofd.txt";

static int fresh_fd(int flags) {
  int fd = open(g_path, flags);
  TEST_ASSERT_TRUE(fd >= 0);
  return fd;
}

static int ofd_wrlck(int fd, long start, long len) {
  struct flock lk = {0};
  lk.l_type = F_WRLCK;
  lk.l_whence = SEEK_SET;
  lk.l_start = start;
  lk.l_len = len;
  return fcntl(fd, F_OFD_SETLK, &lk);
}

static int ofd_unlk(int fd, long start, long len) {
  struct flock lk = {0};
  lk.l_type = F_UNLCK;
  lk.l_whence = SEEK_SET;
  lk.l_start = start;
  lk.l_len = len;
  return fcntl(fd, F_OFD_SETLK, &lk);
}

static int posix_wrlck(int fd, long start, long len) {
  struct flock lk = {0};
  lk.l_type = F_WRLCK;
  lk.l_whence = SEEK_SET;
  lk.l_start = start;
  lk.l_len = len;
  return fcntl(fd, F_SETLK, &lk);
}

/* Two independent opens by the same process: OFD write locks on overlapping
 * ranges conflict (-EAGAIN). */
void test_ofd_independent_fds_conflict(void) {
  int fd1 = fresh_fd(O_RDWR | O_CREAT);
  int fd2 = fresh_fd(O_RDWR);

  TEST_ASSERT_EQUAL_INT(0, ofd_wrlck(fd1, 0, 16));
  int r = ofd_wrlck(fd2, 0, 16);
  TEST_ASSERT_EQUAL_INT(-1, r);
  TEST_ASSERT_EQUAL_INT(EAGAIN, errno);

  ofd_unlk(fd1, 0, 16);
  close(fd1);
  close(fd2);
}

/* Contrast: POSIX F_SETLK on the same two independent fds does NOT conflict
 * (per-process: same pid never conflicts). This is the behavior OFD changes. */
void test_posix_same_pid_no_conflict_contrast(void) {
  int fd1 = fresh_fd(O_RDWR | O_CREAT);
  int fd2 = fresh_fd(O_RDWR);

  TEST_ASSERT_EQUAL_INT(0, posix_wrlck(fd1, 0, 16));
  TEST_ASSERT_EQUAL_INT(0, posix_wrlck(fd2, 0, 16)); /* same pid → OK */

  struct flock unl = {0};
  unl.l_type = F_UNLCK;
  unl.l_whence = SEEK_SET;
  unl.l_start = 0;
  unl.l_len = 0;
  fcntl(fd1, F_SETLK, &unl);
  close(fd1);
  close(fd2);
}

/* dup()'d fds share one open file description → OFD locks do not conflict
 * between them. */
void test_ofd_dup_shares_lock(void) {
  int fd1 = fresh_fd(O_RDWR | O_CREAT);
  int fd2 = dup(fd1);
  TEST_ASSERT_TRUE(fd2 >= 0);

  TEST_ASSERT_EQUAL_INT(0, ofd_wrlck(fd1, 0, 16));
  TEST_ASSERT_EQUAL_INT(0, ofd_wrlck(fd2, 0, 16)); /* shared description */

  ofd_unlk(fd1, 0, 16);
  close(fd1);
  close(fd2);
}

/* F_OFD_GETLK reports the conflicting holder: l_type=F_WRLCK, l_pid=creator. */
void test_ofd_getlk_reports_conflict(void) {
  int fd1 = fresh_fd(O_RDWR | O_CREAT);
  int fd2 = fresh_fd(O_RDWR);

  TEST_ASSERT_EQUAL_INT(0, ofd_wrlck(fd1, 4, 8));

  struct flock probe = {0};
  probe.l_type = F_WRLCK;
  probe.l_whence = SEEK_SET;
  probe.l_start = 0;
  probe.l_len = 16;
  TEST_ASSERT_EQUAL_INT(0, fcntl(fd2, F_OFD_GETLK, &probe));
  TEST_ASSERT_EQUAL_INT(F_WRLCK, probe.l_type);
  TEST_ASSERT_EQUAL_INT(getpid(), probe.l_pid);
  TEST_ASSERT_EQUAL_INT(4, probe.l_start);
  TEST_ASSERT_EQUAL_INT(8, probe.l_len);

  ofd_unlk(fd1, 4, 8);
  close(fd1);
  close(fd2);
}

/* F_OFD_GETLK with no conflict returns F_UNLCK. */
void test_ofd_getlk_unlocked(void) {
  int fd = fresh_fd(O_RDWR | O_CREAT);

  struct flock probe = {0};
  probe.l_type = F_WRLCK;
  probe.l_whence = SEEK_SET;
  probe.l_start = 0;
  probe.l_len = 16;
  TEST_ASSERT_EQUAL_INT(0, fcntl(fd, F_OFD_GETLK, &probe));
  TEST_ASSERT_EQUAL_INT(F_UNLCK, probe.l_type);
  TEST_ASSERT_EQUAL_INT(0, probe.l_pid);

  close(fd);
}

/* Closing the last fd of a description releases its OFD locks: a fresh open
 * can then lock the same range without conflict. */
void test_ofd_close_releases(void) {
  int fd1 = fresh_fd(O_RDWR | O_CREAT);
  TEST_ASSERT_EQUAL_INT(0, ofd_wrlck(fd1, 0, 16));
  close(fd1); /* last fd of this description → OFD lock released */

  int fd2 = fresh_fd(O_RDWR);
  TEST_ASSERT_EQUAL_INT(0, ofd_wrlck(fd2, 0, 16)); /* no lingering conflict */

  ofd_unlk(fd2, 0, 16);
  close(fd2);
}

/* Cross-process OFD conflict: parent locks, child's independent open fails
 * with -EAGAIN (mirrors POSIX cross-process behavior, but exercises the OFD
 * path end to end). */
void test_ofd_cross_process_conflict(void) {
  int fd = fresh_fd(O_RDWR | O_CREAT);
  write(fd, "x", 1);
  TEST_ASSERT_EQUAL_INT(0, ofd_wrlck(fd, 0, 0));

  pid_t pid = fork();
  TEST_ASSERT_TRUE(pid >= 0);
  if (pid == 0) {
    int cfd = open(g_path, O_RDWR);
    if (cfd < 0)
      _exit(100);
    int r = ofd_wrlck(cfd, 0, 0);
    if (r == -1 && errno == EAGAIN)
      _exit(0);
    _exit(1);
  }
  int status = 0;
  pid_t w = waitpid(pid, &status, 0);
  TEST_ASSERT_EQUAL_INT(pid, w);
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));

  ofd_unlk(fd, 0, 0);
  close(fd);
}

/* OFD locks and POSIX locks are mutually exclusive across types: a POSIX
 * write lock blocks an overlapping OFD write lock on a different description.
 */
void test_ofd_vs_posix_cross_type_conflict(void) {
  int fd1 = fresh_fd(O_RDWR | O_CREAT);
  int fd2 = fresh_fd(O_RDWR);

  TEST_ASSERT_EQUAL_INT(0, posix_wrlck(fd1, 0, 16));
  int r = ofd_wrlck(fd2, 0, 16);
  TEST_ASSERT_EQUAL_INT(-1, r);
  TEST_ASSERT_EQUAL_INT(EAGAIN, errno);

  struct flock unl = {0};
  unl.l_type = F_UNLCK;
  unl.l_whence = SEEK_SET;
  unl.l_start = 0;
  unl.l_len = 16;
  fcntl(fd1, F_SETLK, &unl);
  close(fd1);
  close(fd2);
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();
  RUN_TEST(test_ofd_independent_fds_conflict);
  RUN_TEST(test_posix_same_pid_no_conflict_contrast);
  RUN_TEST(test_ofd_dup_shares_lock);
  RUN_TEST(test_ofd_getlk_reports_conflict);
  RUN_TEST(test_ofd_getlk_unlocked);
  RUN_TEST(test_ofd_close_releases);
  RUN_TEST(test_ofd_cross_process_conflict);
  RUN_TEST(test_ofd_vs_posix_cross_type_conflict);
  return UNITY_END();
}

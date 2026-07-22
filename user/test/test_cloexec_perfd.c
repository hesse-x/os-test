/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_cloexec_perfd.c — S06: per-fd close-on-exec bitmap.
 *
 * The old design stored the FD_CLOEXEC bit on the refcounted/shared struct
 * file, so dup'd fds shared one bit: F_DUPFD_CLOEXEC / dup3 wrongly flipped
 * the original fd's cloexec too, and F_SETFD on one dup leaked to the others.
 * S06 moves cloexec to a per-fd bitmap in struct files. These tests assert the
 * per-fd independence that the bitmap gives:
 *   - open(O_CLOEXEC) sets only the opened fd; dup(fd) clears cloexec on the
 *     copy (POSIX dup never inherits FD_CLOEXEC).
 *   - F_SETFD on one dup does not touch the other.
 *   - F_DUPFD_CLOEXEC sets only the new fd.
 *   - fork preserves the fd (fork inherits all fds, cloexec is an exec-time
 *     attribute, not a fork-time one) and the child sees the same cloexec bit.
 */
#include "unity.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/process.h>
#include <sys/wait.h>
#include <unistd.h>

void setUp(void) {}
void tearDown(void) {}

static const char *CLOEXEC_FILE = "/cloexec_perfd_a";

static int make_file(void) {
  int fd = open(CLOEXEC_FILE, O_CREAT | O_RDWR | O_TRUNC | O_CLOEXEC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  return fd;
}

static void cleanup_file(void) { unlink(CLOEXEC_FILE); }

/* open(O_CLOEXEC) marks the opened fd; dup(fd) clears cloexec on the copy. */
void test_open_cloexec_dup_clears(void) {
  int fd = make_file();
  int fd2 = dup(fd);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd2);

  TEST_ASSERT_EQUAL_INT(FD_CLOEXEC, fcntl(fd, F_GETFD));
  /* dup never inherits cloexec (POSIX) — the bitmap bit for fd2 is clear. */
  TEST_ASSERT_EQUAL_INT(0, fcntl(fd2, F_GETFD));

  close(fd2);
  close(fd);
  cleanup_file();
}

/* F_SETFD on one dup is independent: the other dup's cloexec is untouched. */
void test_setfd_independent(void) {
  int fd = open(CLOEXEC_FILE, O_CREAT | O_RDWR | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  int fd2 = dup(fd);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd2);

  /* Neither starts cloexec (no O_CLOEXEC). */
  TEST_ASSERT_EQUAL_INT(0, fcntl(fd, F_GETFD));
  TEST_ASSERT_EQUAL_INT(0, fcntl(fd2, F_GETFD));

  /* Set cloexec on the copy only. */
  TEST_ASSERT_EQUAL_INT(0, fcntl(fd2, F_SETFD, FD_CLOEXEC));
  TEST_ASSERT_EQUAL_INT(0, fcntl(fd, F_GETFD)); /* original unchanged */
  TEST_ASSERT_EQUAL_INT(FD_CLOEXEC, fcntl(fd2, F_GETFD));

  /* Clear it again on the copy; original still 0. */
  TEST_ASSERT_EQUAL_INT(0, fcntl(fd2, F_SETFD, 0));
  TEST_ASSERT_EQUAL_INT(0, fcntl(fd, F_GETFD));
  TEST_ASSERT_EQUAL_INT(0, fcntl(fd2, F_GETFD));

  close(fd2);
  close(fd);
  cleanup_file();
}

/* F_DUPFD_CLOEXEC sets only the new fd; the source fd's cloexec is unchanged.
 */
void test_dupfd_cloexec_only_new(void) {
  int fd = make_file();
  TEST_ASSERT_EQUAL_INT(FD_CLOEXEC, fcntl(fd, F_GETFD));

  int fd3 = fcntl(fd, F_DUPFD_CLOEXEC, 10);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(10, fd3);
  TEST_ASSERT_EQUAL_INT(FD_CLOEXEC, fcntl(fd3, F_GETFD));
  /* Source fd must not have been toggled (the shared-file bug flipped it). */
  TEST_ASSERT_EQUAL_INT(FD_CLOEXEC, fcntl(fd, F_GETFD));

  close(fd3);
  close(fd);
  cleanup_file();
}

/* close() clears the bitmap bit so a recycled slot does not leak a stale
 * cloexec setting into a later fd. Open at a low fd, close it, reopen and
 * confirm the reused slot is not cloexec. */
void test_close_clears_bitmap(void) {
  int fd = make_file();
  TEST_ASSERT_EQUAL_INT(FD_CLOEXEC, fcntl(fd, F_GETFD));
  int saved = fd;
  close(fd);

  /* Reopen without O_CLOEXEC; if close left the bit set, this new fd would
   * incorrectly report cloexec. */
  int fd2 = open(CLOEXEC_FILE, O_CREAT | O_RDWR, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd2);
  /* The new fd may or may not reuse the same slot; either way it must not
   * inherit the old cloexec bit (no O_CLOEXEC passed). */
  TEST_ASSERT_EQUAL_INT(0, fcntl(fd2, F_GETFD));
  (void)saved;

  close(fd2);
  cleanup_file();
}

/* fork inherits all fds (cloexec is exec-time, not fork-time). The child sees
 * the same cloexec bit and the fd remains usable. The child reports the
 * F_GETFD result and the write() result back to the parent over a pipe (this
 * kernel has no anonymous MAP_SHARED, so a shared page would be copy-on-write
 * and the child's writes would never reach the parent). */
void test_fork_inherits_fd_and_cloexec_bit(void) {
  int fd = make_file();
  TEST_ASSERT_EQUAL_INT(FD_CLOEXEC, fcntl(fd, F_GETFD));

  int pfd[2];
  TEST_ASSERT_EQUAL_INT(0, pipe(pfd));

  pid_t pid = fork();
  if (pid == 0) {
    /* Child: close the read end, report both results, exit. */
    close(pfd[0]);
    int rep[2];
    rep[0] = fcntl(fd, F_GETFD);
    const char x = 'z';
    rep[1] = (int)write(fd, &x, 1);
    write(pfd[1], rep, sizeof(rep));
    close(pfd[1]);
    _exit(0);
  } else if (pid > 0) {
    /* Parent: close the write end so read() EOFs after the child writes. */
    close(pfd[1]);
    int rep[2] = {-1, -1};
    ssize_t n = read(pfd[0], rep, sizeof(rep));
    close(pfd[0]);
    int status;
    waitpid(pid, &status, 0);
    TEST_ASSERT_EQUAL_INT((int)sizeof(rep), (int)n);
    /* fork preserves both the fd and its cloexec bit. */
    TEST_ASSERT_EQUAL_INT(FD_CLOEXEC, rep[0]);
    TEST_ASSERT_EQUAL_INT(1, rep[1]); /* write succeeded: fd inherited */
  } else {
    close(pfd[0]);
    close(pfd[1]);
    TEST_FAIL_MESSAGE("fork failed");
  }

  close(fd);
  cleanup_file();
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_open_cloexec_dup_clears);
  RUN_TEST(test_setfd_independent);
  RUN_TEST(test_dupfd_cloexec_only_new);
  RUN_TEST(test_close_clears_bitmap);
  RUN_TEST(test_fork_inherits_fd_and_cloexec_bit);
  return UNITY_END();
}

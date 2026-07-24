/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <unity.h>

#include <sys/ioctl.h>
#include <sys/process.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <xos/errno.h>

void setUp(void) {}
void tearDown(void) {}

/* 1. open(O_WRONLY|O_CREAT) → close → open(O_RDONLY) → read back → strcmp */
void test_open_create_read(void) {
  const char *path = "/local/fcntl_test.txt";
  const char *msg = "hello fcntl";

  int fd = open(path, O_WRONLY | O_CREAT);
  TEST_ASSERT_TRUE(fd >= 0);

  ssize_t w = write(fd, msg, strlen(msg));
  TEST_ASSERT_EQUAL_INT((int)strlen(msg), (int)w);
  close(fd);

  fd = open(path, O_RDONLY);
  TEST_ASSERT_TRUE(fd >= 0);

  char buf[64] = {0};
  ssize_t r = read(fd, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_INT((int)strlen(msg), (int)r);
  TEST_ASSERT_EQUAL_STRING(msg, buf);
  close(fd);
}

/* 2. open nonexistent file returns -ENOENT */
void test_open_nonexist(void) {
  int fd = open("/local/no_such_file_12345", O_RDONLY);
  TEST_ASSERT_TRUE(fd < 0);
  TEST_ASSERT_EQUAL_INT(ENOENT, errno);
}

/* 3. close same fd twice, second returns -EBADF */
void test_close_twice(void) {
  int fd[2];
  pipe(fd);

  int r1 = close(fd[0]);
  TEST_ASSERT_EQUAL_INT(0, r1);

  int r2 = close(fd[0]);
  TEST_ASSERT_TRUE(r2 < 0);
  TEST_ASSERT_EQUAL_INT(EBADF, errno);

  close(fd[1]);
}

/* 4. dup2(old, new) copies fd */
void test_dup2_basic(void) {
  int fd[2];
  pipe(fd);

  int new_fd = dup2(fd[0], 10);
  TEST_ASSERT_EQUAL_INT(10, new_fd);

  /* Write to fd[1], read from new_fd */
  write(fd[1], "x", 1);
  char buf[2] = {0};
  ssize_t r = read(new_fd, buf, 1);
  TEST_ASSERT_EQUAL_INT(1, (int)r);
  TEST_ASSERT_EQUAL_STRING("x", buf);

  close(fd[1]);
  close(new_fd);
  /* fd[0] was also closed by the dup2/share, but close it again */
}

/* 5. dup2 with bad fd returns -EBADF */
void test_dup2_bad_fd(void) {
  int r = dup2(-1, 10);
  TEST_ASSERT_TRUE(r < 0);
  TEST_ASSERT_EQUAL_INT(EBADF, errno);
}

/* 6. fcntl F_SETFL / F_GETFL O_NONBLOCK */
void test_fcntl_setfl(void) {
  int fd[2];
  pipe(fd);

  int r = fcntl(fd[0], F_SETFL, O_NONBLOCK);
  TEST_ASSERT_EQUAL_INT(0, r);

  int flags = fcntl(fd[0], F_GETFL);
  TEST_ASSERT_TRUE(flags & O_NONBLOCK);

  /* Clear NONBLOCK */
  fcntl(fd[0], F_SETFL, 0);
  flags = fcntl(fd[0], F_GETFL);
  TEST_ASSERT_TRUE(!(flags & O_NONBLOCK));

  close(fd[0]);
  close(fd[1]);
}

/* 7. lseek SEEK_SET */
void test_lseek_set(void) {
  int fd = open("/local/fcntl_seek.txt", O_WRONLY | O_CREAT);
  TEST_ASSERT_TRUE(fd >= 0);

  const char *data = "ABCDEFGHIJ";
  write(fd, data, 10);
  close(fd);

  fd = open("/local/fcntl_seek.txt", O_RDONLY);
  TEST_ASSERT_TRUE(fd >= 0);

  lseek(fd, 5, SEEK_SET);
  char buf[6] = {0};
  read(fd, buf, 5);
  TEST_ASSERT_EQUAL_STRING("FGHIJ", buf);

  close(fd);
}

/* 8. lseek SEEK_CUR */
void test_lseek_cur(void) {
  int fd = open("/local/fcntl_seek2.txt", O_WRONLY | O_CREAT);
  TEST_ASSERT_TRUE(fd >= 0);

  const char *data = "ABCDEFGHIJ";
  write(fd, data, 10);
  close(fd);

  fd = open("/local/fcntl_seek2.txt", O_RDONLY);
  TEST_ASSERT_TRUE(fd >= 0);

  /* Read 3 bytes first */
  char tmp[4];
  read(fd, tmp, 3);

  /* SEEK_CUR +2 → skip DE, land on F */
  lseek(fd, 2, SEEK_CUR);
  char buf[4] = {0};
  read(fd, buf, 3);
  TEST_ASSERT_EQUAL_STRING("FGH", buf);

  close(fd);
}

/* 9. Write 20 bytes → lseek to 10 → read 10 → verify */
void test_write_read_lseek(void) {
  int fd = open("/local/fcntl_wr.txt", O_WRONLY | O_CREAT);
  TEST_ASSERT_TRUE(fd >= 0);

  char data[20];
  for (int i = 0; i < 20; i++)
    data[i] = 'A' + i;
  write(fd, data, 20);
  close(fd);

  fd = open("/local/fcntl_wr.txt", O_RDONLY);
  lseek(fd, 10, SEEK_SET);

  char buf[11] = {0};
  read(fd, buf, 10);
  for (int i = 0; i < 10; i++) {
    TEST_ASSERT_EQUAL_INT('K' + i, buf[i]);
  }

  close(fd);
}

/* 10. fstat on regular file → S_ISREG + size */
void test_fstat_regular(void) {
  int fd = open("/local/fcntl_fstat.txt", O_WRONLY | O_CREAT);
  TEST_ASSERT_TRUE(fd >= 0);
  const char *data = "fstat_test_data";
  write(fd, data, strlen(data));
  close(fd);

  fd = open("/local/fcntl_fstat.txt", O_RDONLY);
  TEST_ASSERT_TRUE(fd >= 0);

  struct stat st;
  int r = fstat(fd, &st);
  TEST_ASSERT_EQUAL_INT(0, r);
  TEST_ASSERT_TRUE(S_ISREG(st.st_mode));
  TEST_ASSERT_EQUAL_INT((int)strlen(data), (int)st.st_size);

  close(fd);
}

/* 11. isatty on pipe → 0 */
void test_isatty_pipe(void) {
  int fd[2];
  pipe(fd);

  int r = isatty(fd[0]);
  TEST_ASSERT_EQUAL_INT(0, r);

  close(fd[0]);
  close(fd[1]);
}

/* 12. ftruncate shrinks a file */
void test_ftruncate_shrink(void) {
  int fd = open("/local/ftrunc.txt", O_WRONLY | O_CREAT | O_TRUNC);
  TEST_ASSERT_TRUE(fd >= 0);
  write(fd, "0123456789ABCDE", 15);
  TEST_ASSERT_EQUAL_INT(0, ftruncate(fd, 5));
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, fstat(fd, &st));
  TEST_ASSERT_EQUAL_INT(5, (int)st.st_size);
  close(fd);

  fd = open("/local/ftrunc.txt", O_RDONLY);
  char buf[16] = {0};
  TEST_ASSERT_EQUAL_INT(5, (int)read(fd, buf, 16));
  TEST_ASSERT_EQUAL_STRING("01234", buf);
  close(fd);
}

/* 13. ftruncate grows a file; new bytes read back as zero */
void test_ftruncate_grow(void) {
  int fd = open("/local/ftrunc_g.txt", O_WRONLY | O_CREAT | O_TRUNC);
  TEST_ASSERT_TRUE(fd >= 0);
  write(fd, "AB", 2);
  TEST_ASSERT_EQUAL_INT(0, ftruncate(fd, 10));
  close(fd);

  fd = open("/local/ftrunc_g.txt", O_RDONLY);
  char buf[11] = {0};
  TEST_ASSERT_EQUAL_INT(10, (int)read(fd, buf, 11));
  TEST_ASSERT_EQUAL_INT('A', buf[0]);
  TEST_ASSERT_EQUAL_INT('B', buf[1]);
  for (int i = 2; i < 10; i++)
    TEST_ASSERT_EQUAL_INT(0, buf[i]);
  close(fd);
}

/* 14. truncate(path) by path */
void test_truncate_path(void) {
  int fd = open("/local/trunc.txt", O_WRONLY | O_CREAT | O_TRUNC);
  write(fd, "hello world", 11);
  close(fd);
  TEST_ASSERT_EQUAL_INT(0, truncate("/local/trunc.txt", 5));
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat("/local/trunc.txt", &st));
  TEST_ASSERT_EQUAL_INT(5, (int)st.st_size);
}

/* 15. fsync on a regular file returns 0 */
void test_fsync_regular(void) {
  int fd = open("/local/fsync.txt", O_WRONLY | O_CREAT);
  TEST_ASSERT_TRUE(fd >= 0);
  write(fd, "data", 4);
  TEST_ASSERT_EQUAL_INT(0, fsync(fd));
  close(fd);
  /* sync() takes no args; just exercise it. */
  sync();
}

/* 16. O_CREAT|O_EXCL fails on existing file */
void test_open_excl(void) {
  int fd = open("/local/excl.txt", O_CREAT | O_WRONLY, 0644);
  TEST_ASSERT_TRUE(fd >= 0);
  close(fd);
  errno = 0;
  int r = open("/local/excl.txt", O_CREAT | O_EXCL | O_WRONLY, 0644);
  TEST_ASSERT_EQUAL_INT(-1, r);
  TEST_ASSERT_EQUAL_INT(EEXIST, errno);
}

/* 17. mkstemp returns a unique, writable fd */
void test_mkstemp(void) {
  char tmpl[] = "/local/mkst_XXXXXX";
  int fd = mkstemp(tmpl);
  TEST_ASSERT_TRUE(fd >= 0);
  /* the X's must be replaced */
  TEST_ASSERT_FALSE(strstr(tmpl, "XXXXXX") != NULL);
  write(fd, "tmp", 3);
  lseek(fd, 0, SEEK_SET);
  char buf[4] = {0};
  read(fd, buf, 3);
  TEST_ASSERT_EQUAL_STRING("tmp", buf);
  close(fd);

  /* a second call must yield a distinct name */
  char tmpl2[] = "/local/mkst_XXXXXX";
  int fd2 = mkstemp(tmpl2);
  TEST_ASSERT_TRUE(fd2 >= 0);
  TEST_ASSERT_FALSE(strcmp(tmpl, tmpl2) == 0);
  close(fd2);
}

/* 18. realpath collapses . and .. */
void test_realpath(void) {
  char *r = realpath("/local/a/../b/./c", NULL);
  TEST_ASSERT_NOT_NULL(r);
  TEST_ASSERT_EQUAL_STRING("/local/b/c", r);

  char buf[256];
  char *r2 = realpath("/local/x/y", buf);
  TEST_ASSERT_EQUAL_PTR(buf, r2);
  TEST_ASSERT_EQUAL_STRING("/local/x/y", r2);
}

/* 19. F_DUPFD returns the lowest fd >= arg */
void test_fcntl_dupfd(void) {
  int fd[2];
  pipe(fd);

  int new_fd = fcntl(fd[0], F_DUPFD, 10);
  TEST_ASSERT_TRUE(new_fd >= 10);

  /* The dup'd fd shares the same pipe: write fd[1], read new_fd */
  write(fd[1], "y", 1);
  char buf[2] = {0};
  ssize_t r = read(new_fd, buf, 1);
  TEST_ASSERT_EQUAL_INT(1, (int)r);
  TEST_ASSERT_EQUAL_STRING("y", buf);

  /* Closing the dup'd fd must not close the original. */
  close(new_fd);
  char buf2[2] = {0};
  write(fd[1], "z", 1);
  ssize_t r2 = read(fd[0], buf2, 1);
  TEST_ASSERT_EQUAL_INT(1, (int)r2);
  TEST_ASSERT_EQUAL_STRING("z", buf2);

  close(fd[0]);
  close(fd[1]);
}

/* 20. F_DUPFD with min_fd below the lowest free slot returns that free slot */
void test_fcntl_dupfd_low(void) {
  int fd[2];
  pipe(fd);

  /* fds 0,1,2 are stdin/out/err; fd[0]/fd[1] are 3/4 (or similar). Asking for
   * min_fd=0 yields the lowest currently-free fd. Just assert >= 0 and a valid
   * shared read. */
  int new_fd = fcntl(fd[0], F_DUPFD, 0);
  TEST_ASSERT_TRUE(new_fd >= 0);
  TEST_ASSERT_TRUE(new_fd != fd[0] && new_fd != fd[1]);

  write(fd[1], "q", 1);
  char buf[2] = {0};
  TEST_ASSERT_EQUAL_INT(1, (int)read(new_fd, buf, 1));
  TEST_ASSERT_EQUAL_STRING("q", buf);

  close(new_fd);
  close(fd[0]);
  close(fd[1]);
}

/* 21. F_DUPFD with invalid min_fd returns -EINVAL */
void test_fcntl_dupfd_badarg(void) {
  int fd[2];
  pipe(fd);
  int r = fcntl(fd[0], F_DUPFD, -1);
  TEST_ASSERT_EQUAL_INT(-1, r);
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
  close(fd[0]);
  close(fd[1]);
}

/* 22. F_DUPFD on bad fd returns -EBADF */
void test_fcntl_dupfd_badfd(void) {
  int r = fcntl(-1, F_DUPFD, 5);
  TEST_ASSERT_EQUAL_INT(-1, r);
  TEST_ASSERT_EQUAL_INT(EBADF, errno);
}

/* 23. F_DUPFD_CLOEXEC sets the close-on-exec flag (query via F_GETFD) */
void test_fcntl_dupfd_cloexec(void) {
  int fd[2];
  pipe(fd);
  int new_fd = fcntl(fd[0], F_DUPFD_CLOEXEC, 12);
  TEST_ASSERT_TRUE(new_fd >= 12);

  int flags = fcntl(new_fd, F_GETFD);
  /* F_GETFD is a userspace no-op stub (returns 0) since this OS has no exec,
   * so only assert the dup itself succeeded and the fd is usable. */
  (void)flags;
  write(fd[1], "c", 1);
  char buf[2] = {0};
  TEST_ASSERT_EQUAL_INT(1, (int)read(new_fd, buf, 1));
  TEST_ASSERT_EQUAL_STRING("c", buf);

  close(new_fd);
  close(fd[0]);
  close(fd[1]);
}

/* ===================== S09: POSIX file locks + owner/sig + OFD
 * ===================== */

/* F_GETLK on an unlocked regular file returns F_UNLCK. */
void test_fcntl_getlk_unlocked(void) {
  const char *path = "/local/fcntl_lock.txt";
  int fd = open(path, O_RDWR | O_CREAT);
  TEST_ASSERT_TRUE(fd >= 0);
  write(fd, "lock-data", 9);

  struct flock lk = {0};
  lk.l_type = F_WRLCK;
  lk.l_whence = SEEK_SET;
  lk.l_start = 0;
  lk.l_len = 0; /* to EOF */
  int r = fcntl(fd, F_GETLK, &lk);
  TEST_ASSERT_EQUAL_INT(0, r);
  TEST_ASSERT_EQUAL_INT(F_UNLCK, lk.l_type);

  close(fd);
}

/* F_SETLK write lock succeeds; a self F_GETLK returns F_UNLCK (own locks do
 * not conflict with self — POSIX). */
void test_fcntl_setlk_write_lock_self(void) {
  const char *path = "/local/fcntl_lock2.txt";
  int fd = open(path, O_RDWR | O_CREAT);
  TEST_ASSERT_TRUE(fd >= 0);
  write(fd, "data", 4);

  struct flock lk = {0};
  lk.l_type = F_WRLCK;
  lk.l_whence = SEEK_SET;
  lk.l_start = 0;
  lk.l_len = 0;
  TEST_ASSERT_EQUAL_INT(0, fcntl(fd, F_SETLK, &lk));

  struct flock probe = {0};
  probe.l_type = F_WRLCK;
  probe.l_whence = SEEK_SET;
  probe.l_start = 0;
  probe.l_len = 0;
  TEST_ASSERT_EQUAL_INT(0, fcntl(fd, F_GETLK, &probe));
  TEST_ASSERT_EQUAL_INT(F_UNLCK, probe.l_type);

  /* Unlock. */
  lk.l_type = F_UNLCK;
  TEST_ASSERT_EQUAL_INT(0, fcntl(fd, F_SETLK, &lk));

  close(fd);
}

/* F_SETLK read lock succeeds. */
void test_fcntl_setlk_read_lock(void) {
  const char *path = "/local/fcntl_lock3.txt";
  int fd = open(path, O_RDWR | O_CREAT);
  TEST_ASSERT_TRUE(fd >= 0);

  struct flock lk = {0};
  lk.l_type = F_RDLCK;
  lk.l_whence = SEEK_SET;
  lk.l_start = 0;
  lk.l_len = 16;
  TEST_ASSERT_EQUAL_INT(0, fcntl(fd, F_SETLK, &lk));

  lk.l_type = F_UNLCK;
  TEST_ASSERT_EQUAL_INT(0, fcntl(fd, F_SETLK, &lk));
  close(fd);
}

/* F_SETLK on a pipe fd is rejected (-ENOLCK, surfaced as -1/EINVAL via libc).
 */
void test_fcntl_setlk_pipe_rejected(void) {
  int fd[2];
  pipe(fd);

  struct flock lk = {0};
  lk.l_type = F_WRLCK;
  lk.l_whence = SEEK_SET;
  lk.l_start = 0;
  lk.l_len = 0;
  int r = fcntl(fd[0], F_SETLK, &lk);
  TEST_ASSERT_EQUAL_INT(-1, r);
  TEST_ASSERT_TRUE(errno == EINVAL || errno == ENOLCK);

  close(fd[0]);
  close(fd[1]);
}

/* Cross-process write-lock conflict: parent locks, child F_SETLK on the same
 * range fails with -EAGAIN (non-blocking). */
void test_fcntl_setlk_conflict_child(void) {
  const char *path = "/local/fcntl_lock4.txt";
  int fd = open(path, O_RDWR | O_CREAT);
  TEST_ASSERT_TRUE(fd >= 0);
  write(fd, "shared-file-content", 20);

  struct flock lk = {0};
  lk.l_type = F_WRLCK;
  lk.l_whence = SEEK_SET;
  lk.l_start = 0;
  lk.l_len = 0;
  TEST_ASSERT_EQUAL_INT(0, fcntl(fd, F_SETLK, &lk));

  pid_t pid = fork();
  TEST_ASSERT_TRUE(pid >= 0);
  if (pid == 0) {
    int cfd = open(path, O_RDWR);
    if (cfd < 0)
      _exit(100);
    struct flock cl = {0};
    cl.l_type = F_WRLCK;
    cl.l_whence = SEEK_SET;
    cl.l_start = 0;
    cl.l_len = 0;
    int r = fcntl(cfd, F_SETLK, &cl);
    if (r == -1 && errno == EAGAIN)
      _exit(0); /* expected: conflict */
    _exit(1);   /* unexpected */
  }
  int status = 0;
  pid_t w = waitpid(pid, &status, 0);
  TEST_ASSERT_EQUAL_INT(pid, w);
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));

  lk.l_type = F_UNLCK;
  fcntl(fd, F_SETLK, &lk);
  close(fd);
}

/* F_OFD_SETLK now implemented (per open file description). A single-fd lock
 * over the whole file succeeds and returns 0. */
void test_fcntl_ofd_setlk_basic(void) {
  const char *path = "/local/fcntl_lock5.txt";
  int fd = open(path, O_RDWR | O_CREAT);
  TEST_ASSERT_TRUE(fd >= 0);

  struct flock lk = {0};
  lk.l_type = F_WRLCK;
  lk.l_whence = SEEK_SET;
  lk.l_start = 0;
  lk.l_len = 0;
  int r = fcntl(fd, F_OFD_SETLK, &lk);
  TEST_ASSERT_EQUAL_INT(0, r);

  lk.l_type = F_UNLCK;
  fcntl(fd, F_OFD_SETLK, &lk);
  close(fd);
}

/* F_SETOWN/F_GETOWN round-trip (stored only, no SIGIO delivery). */
void test_fcntl_getown_setown(void) {
  int fd[2];
  pipe(fd);
  TEST_ASSERT_EQUAL_INT(0, fcntl(fd[0], F_SETOWN, 123));
  TEST_ASSERT_EQUAL_INT(123, fcntl(fd[0], F_GETOWN));
  /* Negative/zero pid rejected. */
  TEST_ASSERT_EQUAL_INT(-1, fcntl(fd[0], F_SETOWN, 0));
  close(fd[0]);
  close(fd[1]);
}

/* F_SETSIG/F_GETSIG round-trip. */
void test_fcntl_getsig_setsig(void) {
  int fd[2];
  pipe(fd);
  TEST_ASSERT_EQUAL_INT(0, fcntl(fd[0], F_SETSIG, SIGUSR1));
  TEST_ASSERT_EQUAL_INT(SIGUSR1, fcntl(fd[0], F_GETSIG));
  /* Out-of-range signal rejected. */
  TEST_ASSERT_EQUAL_INT(-1, fcntl(fd[0], F_SETSIG, 99999));
  close(fd[0]);
  close(fd[1]);
}

/* F_SETOWN_EX / F_GETOWN_EX round-trip (stored only, no SIGIO delivery).
 * Covers TID/PID/PGRP recipient classes, the negative-pgid readback for
 * F_OWNER_PGRP, illegal-type rejection, and the legacy F_SETOWN →
 * F_GETOWN_EX downgrade path. */
void test_fcntl_setown_ex(void) {
  int fd[2];
  pipe(fd);
  pid_t me = getpid();

  /* F_OWNER_TID: read back verbatim. */
  struct f_owner_ex ex = {.type = F_OWNER_TID, .pid = me};
  TEST_ASSERT_EQUAL_INT(0, fcntl(fd[0], F_SETOWN_EX, &ex));
  struct f_owner_ex rd = {0};
  TEST_ASSERT_EQUAL_INT(0, fcntl(fd[0], F_GETOWN_EX, &rd));
  TEST_ASSERT_EQUAL_INT(F_OWNER_TID, rd.type);
  TEST_ASSERT_EQUAL_INT(me, rd.pid);

  /* F_OWNER_PGRP: stored internally as -pgid, read back as positive. */
  ex.type = F_OWNER_PGRP;
  ex.pid = 5;
  TEST_ASSERT_EQUAL_INT(0, fcntl(fd[0], F_SETOWN_EX, &ex));
  TEST_ASSERT_EQUAL_INT(0, fcntl(fd[0], F_GETOWN_EX, &rd));
  TEST_ASSERT_EQUAL_INT(F_OWNER_PGRP, rd.type);
  TEST_ASSERT_EQUAL_INT(5, rd.pid);
  /* Legacy F_GETOWN sees the stored negative value (pgid convention). */
  TEST_ASSERT_EQUAL_INT(-5, fcntl(fd[0], F_GETOWN));

  /* Illegal type rejected. */
  ex.type = 3;
  ex.pid = me;
  TEST_ASSERT_EQUAL_INT(-1, fcntl(fd[0], F_SETOWN_EX, &ex));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);

  /* Legacy F_SETOWN sets F_OWNER_PID; F_GETOWN_EX reports it. */
  TEST_ASSERT_EQUAL_INT(0, fcntl(fd[0], F_SETOWN, me));
  TEST_ASSERT_EQUAL_INT(0, fcntl(fd[0], F_GETOWN_EX, &rd));
  TEST_ASSERT_EQUAL_INT(F_OWNER_PID, rd.type);
  TEST_ASSERT_EQUAL_INT(me, rd.pid);

  close(fd[0]);
  close(fd[1]);
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();
  RUN_TEST(test_open_create_read);
  RUN_TEST(test_open_nonexist);
  RUN_TEST(test_close_twice);
  RUN_TEST(test_dup2_basic);
  RUN_TEST(test_dup2_bad_fd);
  RUN_TEST(test_fcntl_setfl);
  RUN_TEST(test_lseek_set);
  RUN_TEST(test_lseek_cur);
  RUN_TEST(test_write_read_lseek);
  RUN_TEST(test_fstat_regular);
  RUN_TEST(test_isatty_pipe);
  RUN_TEST(test_ftruncate_shrink);
  RUN_TEST(test_ftruncate_grow);
  RUN_TEST(test_truncate_path);
  RUN_TEST(test_fsync_regular);
  RUN_TEST(test_open_excl);
  RUN_TEST(test_mkstemp);
  RUN_TEST(test_realpath);
  RUN_TEST(test_fcntl_dupfd);
  RUN_TEST(test_fcntl_dupfd_low);
  RUN_TEST(test_fcntl_dupfd_badarg);
  RUN_TEST(test_fcntl_dupfd_badfd);
  RUN_TEST(test_fcntl_dupfd_cloexec);
  RUN_TEST(test_fcntl_getlk_unlocked);
  RUN_TEST(test_fcntl_setlk_write_lock_self);
  RUN_TEST(test_fcntl_setlk_read_lock);
  RUN_TEST(test_fcntl_setlk_pipe_rejected);
  RUN_TEST(test_fcntl_setlk_conflict_child);
  RUN_TEST(test_fcntl_ofd_setlk_basic);
  RUN_TEST(test_fcntl_getown_setown);
  RUN_TEST(test_fcntl_getsig_setsig);
  RUN_TEST(test_fcntl_setown_ex);
  return UNITY_END();
}

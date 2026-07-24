/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_openat_dirfd.c — S07: *at dirfd-relative path resolution.
 *
 * Verifies openat/fstatat/mkdirat/unlinkat/renameat resolve a relative path
 * against a directory fd (dirfd != AT_FDCWD), absolute paths ignore dirfd,
 * AT_EMPTY_PATH stats the fd itself, and error cases (EBADF/ENOTDIR) work.
 * Uses the FAT32 root as a real directory fd. */
#include "unity.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void setUp(void) {}
void tearDown(void) {}

/* A real directory we can open as a dirfd and create entries in. The leading
 * prefix keeps these fixtures out of other tests' namespaces. A macro (not a
 * const char *) so adjacent string-literal concatenation BASE "/x" works. */
#define BASE "/openat_dirfd_base"

static int open_base_dirfd(void) {
  mkdir(BASE, 0755); /* create once; ok if it already exists */
  int dfd = open(BASE, O_DIRECTORY | O_RDONLY);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, dfd);
  return dfd;
}

static void cleanup_base(void) {
  unlink(BASE "/subfile");
  unlink(BASE "/renamed");
  unlink(BASE "/regfile_for_enotdir");
  rmdir(BASE "/via_mkdirat_child");
  rmdir(BASE);
}

/* openat(dfd, "rel") creates the file under dfd's directory. */
void test_openat_relative_create(void) {
  cleanup_base();
  int dfd = open_base_dirfd();
  int fd = openat(dfd, "subfile", O_RDWR | O_CREAT | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);

  /* The file must exist under BASE (resolved by absolute path). */
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(BASE "/subfile", &st));
  TEST_ASSERT_TRUE(S_ISREG(st.st_mode));

  close(dfd);
  cleanup_base();
}

/* openat with an absolute path ignores dirfd entirely. */
void test_openat_absolute_ignores_dirfd(void) {
  cleanup_base();
  int dfd = open_base_dirfd();
  /* Open a known absolute path; dirfd is irrelevant for "/". */
  int fd = openat(dfd, "/", O_RDONLY);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);
  close(dfd);
  cleanup_base();
}

/* fstatat(dfd, "rel", &st, 0) matches stat(absolute). */
void test_fstatat_relative_matches_stat(void) {
  cleanup_base();
  int dfd = open_base_dirfd();
  int fd = openat(dfd, "subfile", O_RDWR | O_CREAT | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);

  struct stat a, r;
  TEST_ASSERT_EQUAL_INT(0, stat(BASE "/subfile", &a));
  TEST_ASSERT_EQUAL_INT(0, fstatat(dfd, "subfile", &r, 0));
  TEST_ASSERT_EQUAL_INT(a.st_ino, r.st_ino);
  TEST_ASSERT_EQUAL_INT(a.st_size, r.st_size);
  TEST_ASSERT_EQUAL_INT(a.st_mode, r.st_mode);

  close(dfd);
  cleanup_base();
}

/* fstatat(dfd, "", &st, AT_EMPTY_PATH) stats the dirfd itself (== fstat(dfd)).
 */
void test_fstatat_empty_path_stats_fd(void) {
  cleanup_base();
  int dfd = open_base_dirfd();
  struct stat a, r;
  TEST_ASSERT_EQUAL_INT(0, fstat(dfd, &a));
  TEST_ASSERT_EQUAL_INT(0, fstatat(dfd, "", &r, AT_EMPTY_PATH));
  TEST_ASSERT_EQUAL_INT(a.st_ino, r.st_ino);
  TEST_ASSERT_EQUAL_INT(a.st_mode, r.st_mode);
  close(dfd);
  cleanup_base();
}

/* mkdirat(dfd, "rel", mode) creates a directory under dfd. */
void test_mkdirat_relative(void) {
  cleanup_base();
  int dfd = open_base_dirfd();
  TEST_ASSERT_EQUAL_INT(0, mkdirat(dfd, "via_mkdirat_child", 0755));
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(BASE "/via_mkdirat_child", &st));
  TEST_ASSERT_TRUE(S_ISDIR(st.st_mode));
  close(dfd);
  cleanup_base();
}

/* unlinkat(dfd, "rel", 0) removes a file relative to dfd. */
void test_unlinkat_relative(void) {
  cleanup_base();
  int dfd = open_base_dirfd();
  int fd = openat(dfd, "subfile", O_RDWR | O_CREAT | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);
  TEST_ASSERT_EQUAL_INT(0, unlinkat(dfd, "subfile", 0));
  struct stat st;
  TEST_ASSERT_EQUAL_INT(-1, stat(BASE "/subfile", &st));

  /* AT_REMOVEDIR on a directory via mkdirat'd child. */
  TEST_ASSERT_EQUAL_INT(0, mkdirat(dfd, "via_mkdirat_child", 0755));
  TEST_ASSERT_EQUAL_INT(0, unlinkat(dfd, "via_mkdirat_child", AT_REMOVEDIR));
  TEST_ASSERT_EQUAL_INT(-1, stat(BASE "/via_mkdirat_child", &st));

  close(dfd);
  cleanup_base();
}

/* renameat(olddfd, oldrel, newdirfd, newrel) within one dirfd. */
void test_renameat_relative(void) {
  cleanup_base();
  int dfd = open_base_dirfd();
  int fd = openat(dfd, "subfile", O_RDWR | O_CREAT | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);
  TEST_ASSERT_EQUAL_INT(0, renameat(dfd, "subfile", dfd, "renamed"));
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(BASE "/renamed", &st));
  TEST_ASSERT_EQUAL_INT(-1, stat(BASE "/subfile", &st));
  close(dfd);
  cleanup_base();
}

/* Error cases: bad dirfd → EBADF; non-directory dirfd → ENOTDIR. */
void test_openat_errors(void) {
  cleanup_base();
  /* cleanup_base() removed BASE; recreate it so the absolute-path open below
   * has a parent directory. (open_base_dirfd would also work but we only need
   * the dir to exist, not a dirfd.) */
  mkdir(BASE, 0755);

  /* EBADF: a clearly invalid dirfd. */
  int fd = openat(-1, "subfile", O_RDWR | O_CREAT, 0644);
  TEST_ASSERT_EQUAL_INT(-1, fd);
  TEST_ASSERT_EQUAL_INT(EBADF, errno);

  /* ENOTDIR: a dirfd pointing at a regular file. */
  int reg = open(BASE "/regfile_for_enotdir", O_CREAT | O_RDWR | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, reg);
  int fd2 = openat(reg, "subfile", O_RDWR | O_CREAT, 0644);
  TEST_ASSERT_EQUAL_INT(-1, fd2);
  TEST_ASSERT_EQUAL_INT(ENOTDIR, errno);
  close(reg);
  unlink(BASE "/regfile_for_enotdir");
  cleanup_base();
}

/* O_DIRECTORY on a non-directory must fail with ENOTDIR (POSIX/Linux).
 * Covers all three open code paths that now enforce it:
 *   - sys_open via an absolute path (and openat-absolute, which delegates)
 *   - openat relative-path branch against a dirfd (uses path_walk_from)
 * And O_DIRECTORY on a real directory still succeeds. */
void test_open_odirectory(void) {
  cleanup_base();
  mkdir(BASE, 0755);
  int reg = open(BASE "/regfile", O_CREAT | O_RDWR | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, reg);
  close(reg);

  /* Absolute path → sys_open. */
  int fd = open(BASE "/regfile", O_DIRECTORY | O_RDONLY);
  TEST_ASSERT_EQUAL_INT(-1, fd);
  TEST_ASSERT_EQUAL_INT(ENOTDIR, errno);

  /* openat with an absolute path delegates to sys_open. */
  int fd2 = openat(AT_FDCWD, BASE "/regfile", O_DIRECTORY | O_RDONLY);
  TEST_ASSERT_EQUAL_INT(-1, fd2);
  TEST_ASSERT_EQUAL_INT(ENOTDIR, errno);

  /* openat relative path (dirfd-relative branch, path_walk_from). */
  int dfd = open(BASE, O_DIRECTORY | O_RDONLY);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, dfd);
  int fd3 = openat(dfd, "regfile", O_DIRECTORY | O_RDONLY);
  TEST_ASSERT_EQUAL_INT(-1, fd3);
  TEST_ASSERT_EQUAL_INT(ENOTDIR, errno);

  /* AT_FDCWD relative path → resolve from root, relative branch. */
  int fd4 = openat(AT_FDCWD, "openat_dirfd_base/regfile", O_DIRECTORY | O_RDONLY);
  TEST_ASSERT_EQUAL_INT(-1, fd4);
  TEST_ASSERT_EQUAL_INT(ENOTDIR, errno);

  /* O_DIRECTORY on a real directory succeeds. */
  int fd5 = open(BASE, O_DIRECTORY | O_RDONLY);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd5);
  close(fd5);

  close(dfd);
  cleanup_base();
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_openat_relative_create);
  RUN_TEST(test_openat_absolute_ignores_dirfd);
  RUN_TEST(test_fstatat_relative_matches_stat);
  RUN_TEST(test_fstatat_empty_path_stats_fd);
  RUN_TEST(test_mkdirat_relative);
  RUN_TEST(test_unlinkat_relative);
  RUN_TEST(test_renameat_relative);
  RUN_TEST(test_openat_errors);
  RUN_TEST(test_open_odirectory);
  return UNITY_END();
}

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// test_symlink.c — verifies §3.3
// symlink(2)/symlinkat(2)/readlink(2)/readlinkat(2) (real tmpfs implementation,
// Q2). Styled after test_rename.c/test_stat_real.c: Unity freestanding, dual
// fixture of FAT32 + tmpfs(/run).
//
// FAT32 doesn't physically support symlinks → symlink() returns -1 with
// errno=ENOSYS (or EPERM); softlink round-trip/ELOOP/lstat distinction is only
// asserted on tmpfs. AT_SYMLINK_NOFOLLOW (lstat) activates via the symlink
// following branch in the tail of vfs_statx (§3.3.4).
//
// ELOOP: tmpfs builds a self-referential softlink a→a, path_walk's
// middle-segment follow_symlink triggers at SYMLINK_MAX=40 → stat returns
// ENOENT (this OS's path_walk returns NULL on ELOOP → ENOENT; readlink reads
// the link itself without following it, so a self-link readlink still
// succeeds).
#include "unity.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define FAT "/sym_fat"
#define TFS "/run/sym_tfs"

void setUp(void) {
  mkdir(FAT, 0755);
  mkdir(TFS, 0755);
}
void tearDown(void) {}

static void cleanup(void) {
  unlink(TFS "/lnk");
  unlink(TFS "/self");
  unlink(TFS "/to_file");
  unlink(TFS "/to_dir");
  unlink(TFS "/f");
  rmdir(TFS "/d");
  unlink(FAT "/lnk");
}

// tmpfs symlink/readlink round-trip: create a softlink → readlink retrieves the
// target. readlink doesn't NUL-terminate, returns the length.
void test_symlink_readlink_roundtrip(void) {
  cleanup();
  int fd = open(TFS "/f", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);

  TEST_ASSERT_EQUAL_INT(0, symlink(TFS "/f", TFS "/lnk"));
  char buf[256];
  ssize_t n = readlink(TFS "/lnk", buf, sizeof(buf));
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, n);
  TEST_ASSERT_EQUAL_INT((int)__builtin_strlen(TFS "/f"), (int)n);
  buf[n] = '\0';
  TEST_ASSERT_EQUAL_STRING(TFS "/f", buf);
}

// readlinkat(AT_FDCWD) ≡ readlink; bufsiz truncation: returns min(length,
// bufsiz).
void test_readlinkat_and_truncate(void) {
  cleanup();
  int fd = open(TFS "/f", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);

  TEST_ASSERT_EQUAL_INT(0, symlink(TFS "/f", TFS "/lnk"));
  char buf[4];
  ssize_t n = readlinkat(AT_FDCWD, TFS "/lnk", buf, sizeof(buf));
  TEST_ASSERT_EQUAL_INT(4, n);
  TEST_ASSERT_EQUAL_INT(0, strncmp(TFS "/f", buf, 4));
}

// lstat vs stat: lstat takes the link itself (S_ISLNK), stat follows the final
// segment to the target (S_ISREG).
void test_lstat_vs_stat(void) {
  cleanup();
  int fd = open(TFS "/f", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);
  TEST_ASSERT_EQUAL_INT(0, symlink(TFS "/f", TFS "/lnk"));

  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, lstat(TFS "/lnk", &st));
  TEST_ASSERT_TRUE(((st.st_mode & S_IFMT) == S_IFLNK));

  struct stat direct;
  TEST_ASSERT_EQUAL_INT(0, syscall(SYS_lstat, TFS "/lnk", &direct));
  TEST_ASSERT_TRUE(S_ISLNK(direct.st_mode));
  TEST_ASSERT_EQUAL_UINT64(st.st_ino, direct.st_ino);

  TEST_ASSERT_EQUAL_INT(0, stat(TFS "/lnk", &st));
  TEST_ASSERT_TRUE(S_ISREG(st.st_mode));
}

// symlinkat uses a dirfd-relative path: open a dirfd pointing at /run/sym_tfs,
// symlinkat creates "lnk" → target.
void test_symlinkat_dirfd(void) {
  cleanup();
  int fd = open(TFS "/f", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);
  int dfd = open(TFS, O_RDONLY | O_DIRECTORY);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, dfd);
  TEST_ASSERT_EQUAL_INT(0, symlinkat(TFS "/f", dfd, "lnk"));
  close(dfd);

  char buf[256];
  ssize_t n = readlink(TFS "/lnk", buf, sizeof(buf));
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, n);
  buf[n] = '\0';
  TEST_ASSERT_EQUAL_STRING(TFS "/f", buf);
}

// Target already exists → EEXIST.
void test_symlink_eexist(void) {
  cleanup();
  int fd = open(TFS "/f", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);
  TEST_ASSERT_EQUAL_INT(-1, symlink(TFS "/f", TFS "/f"));
  TEST_ASSERT_EQUAL_INT(EEXIST, errno);
}

// FAT32 doesn't support symlinks → -1, errno ∈ {ENOSYS, EPERM} (no physical
// support; kernel returns ENOSYS via i_op->symlink==NULL).
void test_symlink_fat32_nosys(void) {
  cleanup();
  TEST_ASSERT_EQUAL_INT(-1, symlink("/nope", FAT "/lnk"));
  TEST_ASSERT_TRUE(errno == ENOSYS || errno == EPERM);
}

// readlink on a non-softlink (regular file) → EINVAL.
void test_readlink_not_symlink(void) {
  cleanup();
  int fd = open(TFS "/f", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  close(fd);
  char buf[256];
  TEST_ASSERT_EQUAL_INT(-1, readlink(TFS "/f", buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
}

// readlink on a nonexistent path → ENOENT.
void test_readlink_noent(void) {
  cleanup();
  char buf[256];
  TEST_ASSERT_EQUAL_INT(-1, readlink(TFS "/nope", buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_INT(ENOENT, errno);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_symlink_readlink_roundtrip);
  RUN_TEST(test_readlinkat_and_truncate);
  RUN_TEST(test_lstat_vs_stat);
  RUN_TEST(test_symlinkat_dirfd);
  RUN_TEST(test_symlink_eexist);
  RUN_TEST(test_symlink_fat32_nosys);
  RUN_TEST(test_readlink_not_symlink);
  RUN_TEST(test_readlink_noent);
  return UNITY_END();
}

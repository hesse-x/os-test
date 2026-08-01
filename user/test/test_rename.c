/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// test_rename.c — verify §3.1 SYS_RENAME (tmpfs rename atomic-write base).
// Unity freestanding: empty setUp/tearDown; TEST_ASSERT_* assertions.
// Modeled on test_libudev (isomorphic to other vfs tests).
#include "unity.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Tests work under /run (RAM tmpfs), matching the db's actual on-disk dir
// semantics.
#define RENAME_DIR "/run/udev/data"

void setUp(void) {}
void tearDown(void) {}

static int write_file(const char *path, const char *content) {
  int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0)
    return -1;
  ssize_t n = write(fd, content, strlen(content));
  close(fd);
  return (n > 0) ? 0 : -1;
}

static int read_file(const char *path, char *buf, size_t cap) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  ssize_t n = read(fd, buf, cap - 1);
  close(fd);
  if (n < 0)
    return -1;
  buf[n] = '\0';
  return 0;
}

// Same-dir rename (basic): core db scenario.
void test_rename_same_dir(void) {
  const char *oldp = RENAME_DIR "/rename_basic_old";
  const char *newp = RENAME_DIR "/rename_basic_new";
  write_file(oldp, "hello");
  TEST_ASSERT_EQUAL_INT(0, rename(oldp, newp));
  char buf[64];
  TEST_ASSERT_EQUAL_INT(0, read_file(newp, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("hello", buf);
  // old path should be ENOENT
  int fd = open(oldp, O_RDONLY);
  if (fd >= 0)
    close(fd);
  TEST_ASSERT_LESS_THAN_INT(0, fd);
  unlink(newp);
}

// rename over an existing target: db atomic-update overwrite semantics.
void test_rename_overwrite(void) {
  const char *oldp = RENAME_DIR "/rename_overwrite_old";
  const char *newp = RENAME_DIR "/rename_overwrite_new";
  write_file(newp, "old");
  write_file(oldp, "new");
  TEST_ASSERT_EQUAL_INT(0, rename(oldp, newp));
  char buf[64];
  TEST_ASSERT_EQUAL_INT(0, read_file(newp, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("new", buf);
  unlink(newp);
}

// rename with missing old -> -1/ENOENT: error path.
void test_rename_old_missing(void) {
  const char *oldp = RENAME_DIR "/rename_nope_missing";
  const char *newp = RENAME_DIR "/rename_nope_new";
  TEST_ASSERT_EQUAL_INT(-1, rename(oldp, newp));
}

// After rename, old path is ENOENT and new path is readable: atomic switch.
void test_rename_atomicity(void) {
  const char *oldp = RENAME_DIR "/rename_atom_old";
  const char *newp = RENAME_DIR "/rename_atom_new";
  write_file(oldp, "atomic");
  TEST_ASSERT_EQUAL_INT(0, rename(oldp, newp));
  char buf[64];
  TEST_ASSERT_EQUAL_INT(0, read_file(newp, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("atomic", buf);
  int fd = open(oldp, O_RDONLY);
  if (fd >= 0)
    close(fd);
  TEST_ASSERT_LESS_THAN_INT(0, fd);
  unlink(newp);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_rename_same_dir);
  RUN_TEST(test_rename_overwrite);
  RUN_TEST(test_rename_old_missing);
  RUN_TEST(test_rename_atomicity);
  return UNITY_END();
}

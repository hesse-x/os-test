/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE
#include "unity.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void setUp(void) {}
void tearDown(void) {}

static const char *TEST_DIR = "/dirent_complete";
static const char *TEST_FILES[] = {
    "/dirent_complete/zeta",
    "/dirent_complete/alpha",
    "/dirent_complete/middle",
};

static void create_fixtures(void) {
  mkdir(TEST_DIR, 0755);
  for (size_t i = 0; i < sizeof(TEST_FILES) / sizeof(TEST_FILES[0]); i++) {
    int fd = open(TEST_FILES[i], O_CREAT | O_WRONLY | O_TRUNC, 0644);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
    close(fd);
  }
}

static void remove_fixtures(void) {
  for (size_t i = 0; i < sizeof(TEST_FILES) / sizeof(TEST_FILES[0]); i++)
    unlink(TEST_FILES[i]);
  rmdir(TEST_DIR);
}

static int keep_real_entries(const struct dirent *entry) {
  return strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0;
}

void test_scandir_alphasort(void) {
  create_fixtures();

  struct dirent **entries = NULL;
  int count = scandir(TEST_DIR, &entries, keep_real_entries, alphasort);
  TEST_ASSERT_EQUAL_INT(3, count);
  TEST_ASSERT_EQUAL_STRING("alpha", entries[0]->d_name);
  TEST_ASSERT_EQUAL_STRING("middle", entries[1]->d_name);
  TEST_ASSERT_EQUAL_STRING("zeta", entries[2]->d_name);

  for (int i = 0; i < count; i++)
    free(entries[i]);
  free(entries);
  remove_fixtures();
}

static int count_posix_entries(unsigned char *buf, ssize_t len) {
  int count = 0;
  for (ssize_t off = 0; off < len;) {
    struct posix_dent *entry = (struct posix_dent *)(buf + off);
    TEST_ASSERT_GREATER_THAN_INT(0, entry->d_reclen);
    TEST_ASSERT_LESS_OR_EQUAL_INT(len - off, entry->d_reclen);
    off += entry->d_reclen;
    count++;
  }
  return count;
}

static int count_linux_entries(unsigned char *buf, int len) {
  int count = 0;
  for (int off = 0; off < len;) {
    struct dirent *entry = (struct dirent *)(buf + off);
    TEST_ASSERT_GREATER_THAN_INT(0, entry->d_reclen);
    TEST_ASSERT_LESS_OR_EQUAL_INT(len - off, entry->d_reclen);
    off += entry->d_reclen;
    count++;
  }
  return count;
}

void test_posix_and_linux_getdents(void) {
  create_fixtures();
  int fd = open(TEST_DIR, O_RDONLY | O_DIRECTORY);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);

  unsigned char buf[4096];
  ssize_t posix_len = posix_getdents(fd, buf, sizeof(buf), 0);
  TEST_ASSERT_GREATER_THAN_INT(0, posix_len);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(5, count_posix_entries(buf, posix_len));

  TEST_ASSERT_EQUAL_INT(0, lseek(fd, 0, SEEK_SET));
  int linux_len = getdents(fd, (struct dirent *)buf, sizeof(buf));
  TEST_ASSERT_GREATER_THAN_INT(0, linux_len);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(5, count_linux_entries(buf, linux_len));

  errno = 0;
  TEST_ASSERT_EQUAL_INT(-1, posix_getdents(fd, buf, sizeof(buf), 1));
  TEST_ASSERT_EQUAL_INT(EOPNOTSUPP, errno);

  close(fd);
  remove_fixtures();
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_scandir_alphasort);
  RUN_TEST(test_posix_and_linux_getdents);
  return UNITY_END();
}

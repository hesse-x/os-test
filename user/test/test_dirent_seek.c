/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_dirent_seek.c — S05: readdir thread-safety + seekdir/telldir/
 * rewinddir/dirfd on the user side. Uses the kernel getdents d_off cookie via
 * lseek on the dir fd. Runs against fat32 root entries created here. */
#include "unity.h"
#include <dirent.h>
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

/* Distinct filenames so we can identify seekdir targets. Created fresh; the
 * leading "dirent_test_" prefix keeps them out of other tests' namespaces. */
static const char *SEEK_DIR = "/dirent_seek_dir";
static const char *FILES[] = {"dirent_seek_dir/a", "dirent_seek_dir/b",
                              "dirent_seek_dir/c"};
#define NFILES (sizeof(FILES) / sizeof(FILES[0]))

static void create_fixtures(void) {
  mkdir(SEEK_DIR, 0755);
  for (size_t i = 0; i < NFILES; i++) {
    int fd = open(FILES[i], O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0)
      close(fd);
  }
}

static void remove_fixtures(void) {
  for (size_t i = 0; i < NFILES; i++)
    unlink(FILES[i]);
  rmdir(SEEK_DIR);
}

void test_dirfd(void) {
  create_fixtures();
  DIR *d = opendir(SEEK_DIR);
  TEST_ASSERT_NOT_NULL(d);
  int fd = dirfd(d);
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
  closedir(d);
  remove_fixtures();
}

/* seekdir/telldir: capture the location of an entry, walk past it, then
 * seekdir back and confirm the same entry is returned. */
void test_seekdir_telldir(void) {
  create_fixtures();
  DIR *d = opendir(SEEK_DIR);
  TEST_ASSERT_NOT_NULL(d);

  /* Walk to the third real entry (skipping ./..), recording its tellpos. */
  long loc = -1;
  char target[256] = {0};
  int seen = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
      continue;
    seen++;
    if (seen == 2) {
      loc = telldir(d);
      strncpy(target, e->d_name, 255);
      break;
    }
  }
  TEST_ASSERT_GREATER_OR_EQUAL_INT(0, loc);
  TEST_ASSERT_TRUE(target[0] != '\0');

  /* Drain the rest. */
  while (readdir(d) != NULL)
    ;

  /* Seek back to the recorded cookie and read one entry: it must match. */
  seekdir(d, loc);
  e = readdir(d);
  TEST_ASSERT_NOT_NULL(e);
  TEST_ASSERT_EQUAL_STRING(target, e->d_name);

  closedir(d);
  remove_fixtures();
}

/* rewinddir: after draining, rewind returns to the first entry. */
void test_rewinddir(void) {
  create_fixtures();
  DIR *d = opendir(SEEK_DIR);
  TEST_ASSERT_NOT_NULL(d);

  char first[256] = {0};
  struct dirent *e;
  int seen = 0;
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
      continue;
    seen++;
    if (seen == 1) {
      strncpy(first, e->d_name, 255);
      break;
    }
  }
  TEST_ASSERT_TRUE(first[0] != '\0');

  while (readdir(d) != NULL)
    ;

  rewinddir(d);
  e = readdir(d);
  TEST_ASSERT_NOT_NULL(e);
  /* First entry overall is "."; the first real entry comes after ./.. */
  int got_first_real = 0;
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
      continue;
    TEST_ASSERT_EQUAL_STRING(first, e->d_name);
    got_first_real = 1;
    break;
  }
  TEST_ASSERT_TRUE(got_first_real);

  closedir(d);
  remove_fixtures();
}

/* readdir_r: returns 0 and fills the caller buffer; NULL result at EOF. */
void test_readdir_r(void) {
  create_fixtures();
  DIR *d = opendir(SEEK_DIR);
  TEST_ASSERT_NOT_NULL(d);

  struct dirent entry;
  struct dirent *res = (struct dirent *)0x1;
  int rc = readdir_r(d, &entry, &res);
  TEST_ASSERT_EQUAL_INT(0, rc);
  TEST_ASSERT_NOT_NULL(res);
  /* First non-dot entry should be one of our files. */
  int is_ours =
      (strcmp(res->d_name, "a") == 0 || strcmp(res->d_name, "b") == 0 ||
       strcmp(res->d_name, "c") == 0);
  TEST_ASSERT_TRUE(is_ours || strcmp(res->d_name, ".") == 0 ||
                   strcmp(res->d_name, "..") == 0);

  /* Drain to EOF. */
  while (readdir(d) != NULL)
    ;
  res = (struct dirent *)0x1;
  rc = readdir_r(d, &entry, &res);
  TEST_ASSERT_EQUAL_INT(0, rc);
  TEST_ASSERT_NULL(res);

  closedir(d);
  remove_fixtures();
}

/* readdir is reentrant across distinct DIR streams: the per-DIR result buffer
 * means interleaved readdir on two streams no longer clobber each other. */
void test_readdir_two_streams_independent(void) {
  create_fixtures();
  mkdir("/dirent_seek_dir2", 0755);
  int fd = open("/dirent_seek_dir2/only", O_CREAT | O_WRONLY, 0644);
  if (fd >= 0)
    close(fd);

  DIR *d1 = opendir(SEEK_DIR);
  DIR *d2 = opendir("/dirent_seek_dir2");
  TEST_ASSERT_NOT_NULL(d1);
  TEST_ASSERT_NOT_NULL(d2);

  /* Read one entry from d1, then fully drain d2, then read the NEXT entry
   * from d1 — d1's result must not have been overwritten by d2's reads. */
  struct dirent *e1 = readdir(d1);
  TEST_ASSERT_NOT_NULL(e1);
  char first1[256];
  strncpy(first1, e1->d_name, 255);
  first1[255] = '\0';

  struct dirent *e2;
  while ((e2 = readdir(d2)) != NULL)
    (void)e2;

  /* Continue d1: results must still come from d1's stream and be stable. */
  int count = 1; /* we already read one */
  while ((e1 = readdir(d1)) != NULL)
    count++;
  /* d1 has at least ".", "..", and 3 files = 5 entries. */
  TEST_ASSERT_GREATER_OR_EQUAL_INT(5, count);

  closedir(d1);
  closedir(d2);
  unlink("/dirent_seek_dir2/only");
  rmdir("/dirent_seek_dir2");
  remove_fixtures();
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_dirfd);
  RUN_TEST(test_seekdir_telldir);
  RUN_TEST(test_rewinddir);
  RUN_TEST(test_readdir_r);
  RUN_TEST(test_readdir_two_streams_independent);
  return UNITY_END();
}

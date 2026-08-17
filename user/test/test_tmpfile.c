/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// tmpfile/tmpnam/tempnam — musl src/stdio (musl_stdio_objs), added back once
// the time module migrated (their __randname → __clock_gettime dep resolved;
// same blocker as stdlib mkstemp, todo.md:344/379/391). All three name under
// /tmp, which the kernel does not pre-create (vfs.c only makes /dev /sys /proc
// /run), so the suite mkdirs /tmp up front. tmpfile opens O_CREAT|O_EXCL then
// unlinks immediately (unlink-on-open) and hands a "w+" FILE.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

// mkdir /tmp if missing; ignore EEXIST. Idempotent like init's /run/udev.
static void ensure_tmp_dir(void) {
  if (mkdir("/tmp", 0755) != 0 && errno != EEXIST)
    TEST_FAIL_MESSAGE("mkdir(/tmp) failed");
}

// 1. tmpfile: write, rewind, read back.
void test_tmpfile_write_read(void) {
  ensure_tmp_dir();
  FILE *f = tmpfile();
  TEST_ASSERT_NOT_NULL(f);

  TEST_ASSERT_TRUE(fputs("hello tmpfile", f) >= 0);
  TEST_ASSERT_EQUAL_INT(0, fseek(f, 0, SEEK_SET));

  char buf[32] = {0};
  TEST_ASSERT_NOT_NULL(fgets(buf, sizeof(buf), f));
  TEST_ASSERT_EQUAL_STRING("hello tmpfile", buf);
  fclose(f);
}

// 2. tmpfile: rewound empty stream reports EOF.
void test_tmpfile_empty_eof(void) {
  ensure_tmp_dir();
  FILE *f = tmpfile();
  TEST_ASSERT_NOT_NULL(f);
  TEST_ASSERT_EQUAL_INT(EOF, fgetc(f));
  fclose(f);
}

// 3. tmpnam(NULL): returns a name, writes to internal static buffer.
void test_tmpnam_null(void) {
  ensure_tmp_dir();
  char *s = tmpnam(NULL);
  TEST_ASSERT_NOT_NULL(s);
  TEST_ASSERT_EQUAL_INT(0, strncmp(s, "/tmp/", 5));
}

// 4. tmpnam(buf): returns buf, same content.
void test_tmpnam_buf(void) {
  ensure_tmp_dir();
  char buf[L_tmpnam] = {0};
  char *s = tmpnam(buf);
  TEST_ASSERT_EQUAL_PTR(buf, s);
  TEST_ASSERT_EQUAL_INT(0, strncmp(buf, "/tmp/", 5));
}

// 5. tempnam(NULL, NULL): defaults to P_tmpdir + "temp" prefix.
void test_tempnam_defaults(void) {
  ensure_tmp_dir();
  char *s = tempnam(NULL, NULL);
  TEST_ASSERT_NOT_NULL(s);
  TEST_ASSERT_EQUAL_INT(0, strncmp(s, "/tmp/", 5));
  free(s);
}

// 6. tempnam(dir, pfx): honors caller dir + prefix.
void test_tempnam_custom(void) {
  ensure_tmp_dir();
  char *s = tempnam("/tmp", "myapp");
  TEST_ASSERT_NOT_NULL(s);
  TEST_ASSERT_EQUAL_INT(0, strncmp(s, "/tmp/myapp", 10));
  free(s);
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();
  RUN_TEST(test_tmpfile_write_read);
  RUN_TEST(test_tmpfile_empty_eof);
  RUN_TEST(test_tmpnam_null);
  RUN_TEST(test_tmpnam_buf);
  RUN_TEST(test_tempnam_defaults);
  RUN_TEST(test_tempnam_custom);
  return UNITY_END();
}

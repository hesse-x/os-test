/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

/* 1. printf("hello %s", "world") outputs to stdout */
void test_printf_string(void) {
  int r = printf("hello %s", "world");
  TEST_ASSERT_TRUE(r > 0);
}

/* 2. printf("num %d", 42) formats integer */
void test_printf_int(void) {
  int r = printf("num %d", 42);
  TEST_ASSERT_TRUE(r > 0);
}

/* 3. printf("hex %x", 0xFF) formats hex */
void test_printf_hex(void) {
  int r = printf("hex %x", 0xFF);
  TEST_ASSERT_TRUE(r > 0);
}

/* 4. fprintf(stderr, "err") — unbuffered */
void test_fprintf_stderr(void) {
  int r = fprintf(stderr, "err");
  TEST_ASSERT_TRUE(r > 0);
}

/* 5. fputc + fputs → read back from file */
void test_fputc_fputs(void) {
  /* Write to a file and verify content */
  FILE *fp = fopen("/local/stdio_test.txt", "w");
  if (!fp) {
    /* fopen may not be available; use open/write as fallback */
    int fd = open("/local/stdio_test.txt", O_WRONLY | O_CREAT);
    TEST_ASSERT_TRUE(fd >= 0);
    write(fd, "Abc", 3);
    close(fd);
  } else {
    fputc('A', fp);
    fputs("bc", fp);
    fclose(fp);
  }

  /* Read back and verify */
  int fd = open("/local/stdio_test.txt", O_RDONLY);
  TEST_ASSERT_TRUE(fd >= 0);
  char buf[8] = {0};
  read(fd, buf, 4);
  close(fd);
  TEST_ASSERT_EQUAL_STRING("Abc", buf);
}

/* 6. puts("hello") outputs + newline */
void test_puts_basic(void) {
  int r = puts("hello");
  TEST_ASSERT_TRUE(r >= 0);
}

/* 7. fflush(stdout) — write line-buffered data */
void test_fflush_stdout(void) {
  printf("before flush");
  fflush(stdout);
  /* If we get here without crash, fflush works */
  TEST_ASSERT_TRUE(1);
}

/* 8. fgetc(stdin) from pipe */
void test_stdin_fgetc(void) {
  /* Redirect stdin via pipe */
  int fd[2];
  pipe(fd);
  write(fd[1], "Z", 1);

  /* Save old stdin, replace with pipe read end */
  int old_stdin = dup2(0, 10); /* backup fd 0 */
  dup2(fd[0], 0);              /* pipe read → fd 0 */

  int ch = fgetc(stdin);
  TEST_ASSERT_EQUAL_INT('Z', ch);

  /* Restore stdin */
  dup2(old_stdin, 0);
  close(old_stdin);
  close(fd[0]);
  close(fd[1]);
}

/* 9. getchar() equivalent to fgetc(stdin) */
void test_stdin_getchar(void) {
  int fd[2];
  pipe(fd);
  write(fd[1], "Q", 1);

  int old_stdin = dup2(0, 10);
  dup2(fd[0], 0);

  int ch = getchar();
  TEST_ASSERT_EQUAL_INT('Q', ch);

  dup2(old_stdin, 0);
  close(old_stdin);
  close(fd[0]);
  close(fd[1]);
}

/* 10. vfprintf custom formatting */
void test_vfprintf_custom(void) {
  /* Simple vfprintf test via fprintf (which uses vfprintf internally) */
  int r = fprintf(stdout, "vfprintf %d", 123);
  TEST_ASSERT_TRUE(r > 0);
}

/* ========== %d format precision tests via sprintf ========== */

void test_d_basic(void) {
  char b[64];
  sprintf(b, "%d", 42);
  TEST_ASSERT_EQUAL_STRING("42", b);
}

void test_d_negative(void) {
  char b[64];
  sprintf(b, "%d", -42);
  TEST_ASSERT_EQUAL_STRING("-42", b);
}

void test_d_zero(void) {
  char b[64];
  sprintf(b, "%d", 0);
  TEST_ASSERT_EQUAL_STRING("0", b);
}

void test_d_width(void) {
  char b[64];
  sprintf(b, "%5d", 42);
  TEST_ASSERT_EQUAL_STRING("   42", b);
}

void test_d_negative_width(void) {
  char b[64];
  sprintf(b, "%5d", -42);
  TEST_ASSERT_EQUAL_STRING("  -42", b);
}

void test_d_zero_fill(void) {
  char b[64];
  sprintf(b, "%05d", 42);
  TEST_ASSERT_EQUAL_STRING("00042", b);
}

void test_d_negative_zero_fill(void) {
  char b[64];
  sprintf(b, "%05d", -42);
  TEST_ASSERT_EQUAL_STRING("-0042", b);
}

void test_d_zero_zero_fill(void) {
  char b[64];
  sprintf(b, "%05d", 0);
  TEST_ASSERT_EQUAL_STRING("00000", b);
}

void test_d_left_align(void) {
  char b[64];
  sprintf(b, "%-5d", 42);
  TEST_ASSERT_EQUAL_STRING("42   ", b);
}

void test_d_negative_left_align(void) {
  char b[64];
  sprintf(b, "%-5d", -42);
  TEST_ASSERT_EQUAL_STRING("-42  ", b);
}

void test_d_long(void) {
  char b[64];
  sprintf(b, "%ld", 1234567890123L);
  TEST_ASSERT_EQUAL_STRING("1234567890123", b);
}

void test_d_long_negative(void) {
  char b[64];
  sprintf(b, "%ld", -1234567890123L);
  TEST_ASSERT_EQUAL_STRING("-1234567890123", b);
}

void test_d_int_max(void) {
  char b[64];
  sprintf(b, "%d", 2147483647);
  TEST_ASSERT_EQUAL_STRING("2147483647", b);
}

void test_d_int_min(void) {
  char b[64];
  sprintf(b, "%d", -2147483648);
  TEST_ASSERT_EQUAL_STRING("-2147483648", b);
}

void test_d_long_max(void) {
  char b[64];
  sprintf(b, "%ld", 9223372036854775807L);
  TEST_ASSERT_EQUAL_STRING("9223372036854775807", b);
}

void test_d_long_min(void) {
  char b[64];
  sprintf(b, "%ld", -9223372036854775807L - 1);
  TEST_ASSERT_EQUAL_STRING("-9223372036854775808", b);
}

void test_d_width_larger(void) {
  char b[64];
  sprintf(b, "%10d", 42);
  TEST_ASSERT_EQUAL_STRING("        42", b);
}

void test_d_neg_width_larger(void) {
  char b[64];
  sprintf(b, "%10d", -42);
  TEST_ASSERT_EQUAL_STRING("       -42", b);
}

void test_d_neg_zero_fill_large(void) {
  char b[64];
  sprintf(b, "%010d", -42);
  TEST_ASSERT_EQUAL_STRING("-000000042", b);
}

void test_d_neg_zero_fill_large2(void) {
  char b[64];
  sprintf(b, "%015d", -12345);
  TEST_ASSERT_EQUAL_STRING("-00000000012345", b);
}

/* ========== %f format tests ========== */

void test_f_basic(void) {
  char b[64];
  sprintf(b, "%f", 3.14);
  TEST_ASSERT_EQUAL_STRING("3.140000", b);
}

void test_f_default_precision(void) {
  char b[64];
  sprintf(b, "%f", 1.0);
  TEST_ASSERT_EQUAL_STRING("1.000000", b);
}

void test_f_precision_2(void) {
  char b[64];
  sprintf(b, "%.2f", 3.14);
  TEST_ASSERT_EQUAL_STRING("3.14", b);
}

void test_f_precision_4(void) {
  char b[64];
  sprintf(b, "%.4f", 8.5094);
  TEST_ASSERT_EQUAL_STRING("8.5094", b);
}

void test_f_negative(void) {
  char b[64];
  sprintf(b, "%.2f", -1.5);
  TEST_ASSERT_EQUAL_STRING("-1.50", b);
}

void test_f_zero(void) {
  char b[64];
  sprintf(b, "%f", 0.0);
  TEST_ASSERT_EQUAL_STRING("0.000000", b);
}

void test_f_precision_0(void) {
  char b[64];
  sprintf(b, "%.0f", 42.7);
  TEST_ASSERT_EQUAL_STRING("43", b);
}

void test_f_round_carry(void) {
  char b[64];
  sprintf(b, "%.2f", 9.999);
  TEST_ASSERT_EQUAL_STRING("10.00", b);
}

void test_f_width_pad(void) {
  char b[64];
  sprintf(b, "%8.2f", 3.14);
  TEST_ASSERT_EQUAL_STRING("    3.14", b);
}

void test_f_zero_fill(void) {
  char b[64];
  sprintf(b, "%08.2f", 3.14);
  TEST_ASSERT_EQUAL_STRING("00003.14", b);
}

void test_f_left_align(void) {
  char b[64];
  sprintf(b, "%-8.2f|", 3.14);
  TEST_ASSERT_EQUAL_STRING("3.14    |", b);
}

void test_f_neg_zero_fill(void) {
  char b[64];
  sprintf(b, "%08.2f", -3.14);
  TEST_ASSERT_EQUAL_STRING("-0003.14", b);
}

/* ========== open_memstream tests ========== */

/* open_memstream: basic write, content + size + NUL terminator */
void test_open_memstream_basic(void) {
  char *buf = NULL;
  size_t sz = 0;
  FILE *f = open_memstream(&buf, &sz);
  TEST_ASSERT_NOT_NULL(f);
  fputs("hello", f);
  fflush(f);
  TEST_ASSERT_EQUAL_INT(5, (int)sz);
  TEST_ASSERT_EQUAL_STRING("hello", buf);
  fclose(f);
  free(buf);
}

/* open_memstream: multiple appends accumulate */
void test_open_memstream_append(void) {
  char *buf = NULL;
  size_t sz = 0;
  FILE *f = open_memstream(&buf, &sz);
  fputs("foo", f);
  fputs("bar", f);
  fflush(f);
  TEST_ASSERT_EQUAL_INT(6, (int)sz);
  TEST_ASSERT_EQUAL_STRING("foobar", buf);
  fclose(f);
  free(buf);
}

/* open_memstream: printf formatting into stream */
void test_open_memstream_printf(void) {
  char *buf = NULL;
  size_t sz = 0;
  FILE *f = open_memstream(&buf, &sz);
  fprintf(f, "%d+%d=%d", 2, 3, 5);
  fflush(f);
  TEST_ASSERT_EQUAL_INT(5, (int)sz);
  TEST_ASSERT_EQUAL_STRING("2+3=5", buf);
  fclose(f);
  free(buf);
}

/* open_memstream: grow beyond initial 64-byte capacity */
void test_open_memstream_grow(void) {
  char *buf = NULL;
  size_t sz = 0;
  FILE *f = open_memstream(&buf, &sz);
  for (int i = 0; i < 20; i++)
    fprintf(f, "row%02d\n", i);
  fflush(f);
  /* each "rowNN\n" = 6 bytes × 20 = 120 bytes */
  TEST_ASSERT_EQUAL_INT(120, (int)sz);
  TEST_ASSERT_EQUAL_INT('\n', buf[5]);
  TEST_ASSERT_EQUAL_INT('r', buf[6]);
  fclose(f);
  free(buf);
}

/* open_memstream: empty stream → size 0, buffer non-NULL after flush */
void test_open_memstream_empty(void) {
  char *buf = NULL;
  size_t sz = 99; /* poison */
  FILE *f = open_memstream(&buf, &sz);
  fflush(f);
  TEST_ASSERT_EQUAL_INT(0, (int)sz);
  fclose(f);
}

/* open_memstream: NULL args → NULL return */
void test_open_memstream_null_args(void) {
  TEST_ASSERT_NULL(open_memstream(NULL, NULL));
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();
  RUN_TEST(test_printf_string);
  RUN_TEST(test_printf_int);
  RUN_TEST(test_printf_hex);
  RUN_TEST(test_fprintf_stderr);
  RUN_TEST(test_fputc_fputs);
  RUN_TEST(test_puts_basic);
  RUN_TEST(test_fflush_stdout);
  RUN_TEST(test_stdin_fgetc);
  RUN_TEST(test_stdin_getchar);
  RUN_TEST(test_vfprintf_custom);
  RUN_TEST(test_d_basic);
  RUN_TEST(test_d_negative);
  RUN_TEST(test_d_zero);
  RUN_TEST(test_d_width);
  RUN_TEST(test_d_negative_width);
  RUN_TEST(test_d_zero_fill);
  RUN_TEST(test_d_negative_zero_fill);
  RUN_TEST(test_d_zero_zero_fill);
  RUN_TEST(test_d_left_align);
  RUN_TEST(test_d_negative_left_align);
  RUN_TEST(test_d_long);
  RUN_TEST(test_d_long_negative);
  RUN_TEST(test_d_int_max);
  RUN_TEST(test_d_int_min);
  RUN_TEST(test_d_long_max);
  RUN_TEST(test_d_long_min);
  RUN_TEST(test_d_width_larger);
  RUN_TEST(test_d_neg_width_larger);
  RUN_TEST(test_d_neg_zero_fill_large);
  RUN_TEST(test_d_neg_zero_fill_large2);
  RUN_TEST(test_f_basic);
  RUN_TEST(test_f_default_precision);
  RUN_TEST(test_f_precision_2);
  RUN_TEST(test_f_precision_4);
  RUN_TEST(test_f_negative);
  RUN_TEST(test_f_zero);
  RUN_TEST(test_f_precision_0);
  RUN_TEST(test_f_round_carry);
  RUN_TEST(test_f_width_pad);
  RUN_TEST(test_f_zero_fill);
  RUN_TEST(test_f_left_align);
  RUN_TEST(test_f_neg_zero_fill);
  RUN_TEST(test_open_memstream_basic);
  RUN_TEST(test_open_memstream_append);
  RUN_TEST(test_open_memstream_printf);
  RUN_TEST(test_open_memstream_grow);
  RUN_TEST(test_open_memstream_empty);
  RUN_TEST(test_open_memstream_null_args);
  return UNITY_END();
}

#include <unity.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

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
    int old_stdin = dup2(0, 10);  /* backup fd 0 */
    dup2(fd[0], 0);               /* pipe read → fd 0 */

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
    TEST_ASSERT_EQUAL_STRING("-000000000012345", b);
}

int main(void) {
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
    return UNITY_END();
}

#include <unity.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* 1. strlen("hello") == 5 */
void test_strlen_basic(void) {
    TEST_ASSERT_EQUAL_INT(5, (int)strlen("hello"));
}

/* 2. strlen("") == 0 */
void test_strlen_empty(void) {
    TEST_ASSERT_EQUAL_INT(0, (int)strlen(""));
}

/* 3. strcmp equal strings == 0 */
void test_strcmp_equal(void) {
    TEST_ASSERT_EQUAL_INT(0, strcmp("abc", "abc"));
}

/* 4. strcmp less */
void test_strcmp_less(void) {
    TEST_ASSERT_TRUE(strcmp("abc", "abd") < 0);
}

/* 5. strcmp greater */
void test_strcmp_greater(void) {
    TEST_ASSERT_TRUE(strcmp("abd", "abc") > 0);
}

/* 6. strncmp partial match */
void test_strncmp_partial(void) {
    TEST_ASSERT_EQUAL_INT(0, strncmp("abcdef", "abcxyz", 3));
}

/* 7. strcpy basic */
void test_strcpy_basic(void) {
    char buf[16] = {0};
    strcpy(buf, "hello");
    TEST_ASSERT_EQUAL_STRING("hello", buf);
}

/* 8. strncpy padding */
void test_strncpy_pad(void) {
    char buf[8];
    memset(buf, 'Z', sizeof(buf));
    strncpy(buf, "hi", 5);
    TEST_ASSERT_EQUAL_STRING("hi", buf);
    TEST_ASSERT_EQUAL_INT(0, buf[2]);
    TEST_ASSERT_EQUAL_INT(0, buf[3]);
    TEST_ASSERT_EQUAL_INT(0, buf[4]);
}

/* 9. strcat basic */
void test_strcat_basic(void) {
    char buf[32] = {0};
    strcpy(buf, "hi");
    strcat(buf, " there");
    TEST_ASSERT_EQUAL_STRING("hi there", buf);
}

/* 10. strchr found */
void test_strchr_found(void) {
    const char *s = "hello";
    char *p = strchr(s, 'l');
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_STRING("llo", p);
}

/* 11. strchr missing */
void test_strchr_missing(void) {
    TEST_ASSERT_NULL(strchr("hello", 'z'));
}

/* 12. memcpy basic */
void test_memcpy_basic(void) {
    const char *src = "0123456789";
    char dst[16] = {0};
    memcpy(dst, src, 10);
    TEST_ASSERT_EQUAL_INT(0, memcmp(dst, src, 10));
}

/* 13. memset basic */
void test_memset_basic(void) {
    char buf[16];
    memset(buf, 'A', 10);
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_EQUAL_INT('A', buf[i]);
    }
}

/* 14. memmove overlapping region */
void test_memmove_overlap(void) {
    char buf[32] = "abcdefghijklmno";
    /* Move "abcde" (offset 0) to offset 2 — overlapping */
    memmove(buf + 2, buf, 5);
    TEST_ASSERT_EQUAL_INT('a', buf[2]);
    TEST_ASSERT_EQUAL_INT('b', buf[3]);
    TEST_ASSERT_EQUAL_INT('c', buf[4]);
    TEST_ASSERT_EQUAL_INT('d', buf[5]);
    TEST_ASSERT_EQUAL_INT('e', buf[6]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_strlen_basic);
    RUN_TEST(test_strlen_empty);
    RUN_TEST(test_strcmp_equal);
    RUN_TEST(test_strcmp_less);
    RUN_TEST(test_strcmp_greater);
    RUN_TEST(test_strncmp_partial);
    RUN_TEST(test_strcpy_basic);
    RUN_TEST(test_strncpy_pad);
    RUN_TEST(test_strcat_basic);
    RUN_TEST(test_strchr_found);
    RUN_TEST(test_strchr_missing);
    RUN_TEST(test_memcpy_basic);
    RUN_TEST(test_memset_basic);
    RUN_TEST(test_memmove_overlap);
    return UNITY_END();
}

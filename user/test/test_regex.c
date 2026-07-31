/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "unity.h"
#include <fnmatch.h>
#include <regex.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

void test_extended_regex_captures_leftmost_longest_match(void) {
  regex_t re;
  regmatch_t match[2];

  TEST_ASSERT_EQUAL_INT(0, regcomp(&re, "(ab)+", REG_EXTENDED));
  TEST_ASSERT_EQUAL_INT(0, regexec(&re, "xxababyy", 2, match, 0));
  TEST_ASSERT_EQUAL_INT(2, match[0].rm_so);
  TEST_ASSERT_EQUAL_INT(6, match[0].rm_eo);
  TEST_ASSERT_EQUAL_INT(4, match[1].rm_so);
  TEST_ASSERT_EQUAL_INT(6, match[1].rm_eo);
  regfree(&re);
}

void test_regex_nomatch_and_compile_error(void) {
  regex_t re;
  char message[64];

  TEST_ASSERT_EQUAL_INT(0, regcomp(&re, "^hello$", REG_EXTENDED));
  TEST_ASSERT_EQUAL_INT(REG_NOMATCH, regexec(&re, "hello!", 0, NULL, 0));
  regfree(&re);

  int error = regcomp(&re, "[", REG_EXTENDED);
  TEST_ASSERT_EQUAL_INT(REG_EBRACK, error);
  TEST_ASSERT_GREATER_THAN((size_t)1,
                           regerror(error, &re, message, sizeof message));
  TEST_ASSERT_NOT_EQUAL(0, strcmp("No error", message));
}

void test_regex_icase_and_newline(void) {
  regex_t re;

  TEST_ASSERT_EQUAL_INT(0, regcomp(&re, "^abc$", REG_EXTENDED | REG_ICASE));
  TEST_ASSERT_EQUAL_INT(0, regexec(&re, "AbC", 0, NULL, 0));
  regfree(&re);

  TEST_ASSERT_EQUAL_INT(0, regcomp(&re, "^two$", REG_EXTENDED | REG_NEWLINE));
  TEST_ASSERT_EQUAL_INT(0, regexec(&re, "one\ntwo\nthree", 0, NULL, 0));
  regfree(&re);
}

void test_fnmatch_wildcards_classes_and_flags(void) {
  TEST_ASSERT_EQUAL_INT(0, fnmatch("file-?.[ch]", "file-a.c", 0));
  TEST_ASSERT_EQUAL_INT(FNM_NOMATCH, fnmatch("file-?.[ch]", "file-aa.c", 0));
  TEST_ASSERT_EQUAL_INT(0, fnmatch("src/*.c", "src/main.c", FNM_PATHNAME));
  TEST_ASSERT_EQUAL_INT(FNM_NOMATCH,
                        fnmatch("src/*.c", "src/lib/main.c", FNM_PATHNAME));
  TEST_ASSERT_EQUAL_INT(FNM_NOMATCH, fnmatch("*", ".hidden", FNM_PERIOD));
  TEST_ASSERT_EQUAL_INT(0, fnmatch("*.C", "main.c", FNM_CASEFOLD));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_extended_regex_captures_leftmost_longest_match);
  RUN_TEST(test_regex_nomatch_and_compile_error);
  RUN_TEST(test_regex_icase_and_newline);
  RUN_TEST(test_fnmatch_wildcards_classes_and_flags);
  return UNITY_END();
}

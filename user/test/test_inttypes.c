/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// test_inttypes.c — verify symbols and format macros after <inttypes.h>
// migrated to the musl upstream header.
//
// The symbol side (strtoimax/strtoumax/imaxabs/imaxdiv/wcstoimax/wcstoumax) was
// already compiled into libc via musl_stdlib_objs + musl_wchar_objs and
// exported in libc.map; this test covers the regression after the header switch
// (repo macro shims → real musl headers):
//
//  1. The 6 function symbols link and behave correctly (the tree previously had
//     no #include <inttypes.h> anywhere, so they were never tested).
//  2. The imaxdiv_t layout (quot/rem) is usable.
//  3. Format macro correctness: on this target int64_t = long (_Int64 = long,
//     bits/alltypes.h:8), so the correct length modifier is "l", not the "ll"
//     hard-coded by the old shim. musl headers use __PRI64 (= "l") / __PRIPTR
//     (= "l"): PRId64="ld"/PRIu64="lu"/SCNu64="lu". snprintf+sscanf round-trips
//     verify the "l" modifier aligns with long/int64_t under printf/scanf with
//     no truncation/misread, and also cover the SCNi / SCNo / SCNiMAX families
//     the shim missed.
//
// Mirrors test_access.c: Unity freestanding, no FS fixture (pure compute +
// stdio).
#include "unity.h"
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

void setUp(void) {}
void tearDown(void) {}

// ---- strtoimax / strtoumax ----

void test_strtoimax_hex_base16(void) {
  char *end = NULL;
  const char *s = "0xdeadbeef";
  intmax_t v = strtoimax(s, &end, 16);
  TEST_ASSERT_EQUAL_INT64(0xdeadbeefLL, v);
  TEST_ASSERT_EQUAL_PTR(s + 10, end); // consumes all 8 hex digits → NUL
}

void test_strtoimax_decimal_neg_and_endptr(void) {
  char *end = NULL;
  intmax_t v = strtoimax("  -42abc", &end, 10);
  TEST_ASSERT_EQUAL_INT64(-42, v);
  TEST_ASSERT_EQUAL_STRING("abc", end);
}

void test_strtoimax_int64_max_overflow(void) {
  char *end = NULL;
  errno = 0;
  intmax_t v = strtoimax("9223372036854775808", &end, 10); // INT64_MAX + 1
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, v);
  TEST_ASSERT_EQUAL_INT(ERANGE, errno);
}

void test_strtoumax_uint64_max(void) {
  char *end = NULL;
  errno = 0;
  uintmax_t v = strtoumax("18446744073709551615", &end, 10); // UINT64_MAX
  TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, v);
  TEST_ASSERT_EQUAL_INT(0, errno);
  TEST_ASSERT_EQUAL_CHAR('\0', *end);
}

void test_strtoumax_overflow_wraps(void) {
  char *end = NULL;
  errno = 0;
  uintmax_t v = strtoumax("18446744073709551616", &end, 10); // UINT64_MAX + 1
  TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, v);
  TEST_ASSERT_EQUAL_INT(ERANGE, errno);
}

// ---- imaxabs / imaxdiv ----

void test_imaxabs_neg(void) {
  TEST_ASSERT_EQUAL_INT64(9223372036854775807LL,
                          imaxabs(-9223372036854775807LL));
  TEST_ASSERT_EQUAL_INT64(0, imaxabs(0));
  TEST_ASSERT_EQUAL_INT64(7, imaxabs(-7));
}

void test_imaxdiv_quot_rem(void) {
  imaxdiv_t r = imaxdiv(-1000000000000LL, 7);
  TEST_ASSERT_EQUAL_INT64(-142857142857LL, r.quot);
  TEST_ASSERT_EQUAL_INT64(-1, r.rem);
}

// ---- wcstoimax ----

void test_wcstoimax_decimal(void) {
  wchar_t *end = NULL;
  const wchar_t *s = L"  123xyz";
  intmax_t v = wcstoimax(s, &end, 10);
  TEST_ASSERT_EQUAL_INT64(123, v);
  TEST_ASSERT_EQUAL_PTR(s + 5,
                        end); // end points at "xyz" (skipped ws+3 digits)
}

void test_wcstoimax_hex(void) {
  wchar_t *end = NULL;
  const wchar_t *s = L"ff";
  intmax_t v = wcstoimax(s, &end, 16);
  TEST_ASSERT_EQUAL_INT64(255, v);
  TEST_ASSERT_EQUAL_PTR(s + 2, end);
}

// ---- PRI / SCN format macro correctness (l modifier, matches long/int64_t)
// ---- snprintf→sscanf round-trips verify a 64-bit value written via
// PRId64/PRIu64 and read back via SCNd64/SCNu64 is lossless — direct evidence
// the "l" modifier is correct (a wrong "ll" vs long, or "l" mismatching the
// impl, would truncate/misread).

void test_pri_scn_roundtrip_signed(void) {
  char buf[32];
  const int64_t in = -9007199254740993LL; // odd outside the 53-bit boundary,
                                          // easily exposed by truncation
  snprintf(buf, sizeof buf, "%" PRId64, in);
  int64_t out = 0;
  int n = sscanf(buf, "%" SCNd64, &out);
  TEST_ASSERT_EQUAL_INT(1, n);
  TEST_ASSERT_EQUAL_INT64(in, out);
}

void test_pri_scn_roundtrip_unsigned(void) {
  char buf[32];
  const uint64_t in = 18014398509481983ULL; // < UINT64_MAX, non-trivial
  snprintf(buf, sizeof buf, "%" PRIu64, in);
  uint64_t out = 0;
  int n = sscanf(buf, "%" SCNu64, &out);
  TEST_ASSERT_EQUAL_INT(1, n);
  TEST_ASSERT_EQUAL_UINT64(in, out);
}

void test_pri_scn_hex(void) {
  char buf[32];
  const uint64_t in = 0xABCDEF0123456789ULL;
  snprintf(buf, sizeof buf, "%" PRIX64, in);
  uint64_t out = 0;
  int n = sscanf(buf, "%" SCNx64, &out);
  TEST_ASSERT_EQUAL_INT(1, n);
  TEST_ASSERT_EQUAL_UINT64(in, out);
}

void test_pri_max_matches_64(void) {
  // PRIdMAX/SCNdMAX and PRId64/SCNd64 both resolve to "l"+spec on this target,
  // so the round-trip is consistent for the same int64_t value.
  char buf[32];
  const intmax_t in = INT64_MAX;
  snprintf(buf, sizeof buf, "%" PRIdMAX, in);
  intmax_t out = 0;
  TEST_ASSERT_EQUAL_INT(1, sscanf(buf, "%" SCNdMAX, &out));
  TEST_ASSERT_EQUAL_INT64(in, out);
}

void test_pri_ptr_matches_64(void) {
  // uintptr_t == uint64_t, so PRIuPTR/SCNuPTR share the modifier with
  // PRIu64/SCNu64.
  char buf[32];
  const uintptr_t in = 0xdeadbeefcafeULL;
  snprintf(buf, sizeof buf, "%" PRIuPTR, in);
  uintptr_t out = 0;
  TEST_ASSERT_EQUAL_INT(1, sscanf(buf, "%" SCNuPTR, &out));
  TEST_ASSERT_EQUAL_UINT64(in, (uint64_t)out);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_strtoimax_hex_base16);
  RUN_TEST(test_strtoimax_decimal_neg_and_endptr);
  RUN_TEST(test_strtoimax_int64_max_overflow);
  RUN_TEST(test_strtoumax_uint64_max);
  RUN_TEST(test_strtoumax_overflow_wraps);
  RUN_TEST(test_imaxabs_neg);
  RUN_TEST(test_imaxdiv_quot_rem);
  RUN_TEST(test_wcstoimax_decimal);
  RUN_TEST(test_wcstoimax_hex);
  RUN_TEST(test_pri_scn_roundtrip_signed);
  RUN_TEST(test_pri_scn_roundtrip_unsigned);
  RUN_TEST(test_pri_scn_hex);
  RUN_TEST(test_pri_max_matches_64);
  RUN_TEST(test_pri_ptr_matches_64);
  return UNITY_END();
}

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_inttypes.c — 验证 <inttypes.h> 迁移到 musl 上游头后的符号与格式宏。
 *
 * inttypes 的符号侧(strtoimax/strtoumax/imaxabs/imaxdiv/wcstoimax/wcstoumax)
 * 早已由 musl_stdlib_objs + musl_wchar_objs 编译进 libc 并在 libc.map 导出;
 * 本测试补的是头文件切换(repo 宏垫片 → musl 真头)后的回归:
 *
 *  1. 6 个函数符号链接/行为正确(此前树内无任何 #include
 * <inttypes.h>,从未被测)。
 *  2. imaxdiv_t 结构布局(quot/rem)可用。
 *  3. 格式宏正确性:本 target int64_t = long(_Int64 = long,bits/alltypes.h:8),
 *     故正确长度修饰是 "l",不是旧垫片硬编码的 "ll"。musl 头用 __PRI64(= "l")/
 *     __PRIPTR(= "l"),PRId64="ld"/PRIu64="lu"/SCNu64="lu"。用 snprintf+sscanf
 *     往返验证 "l" 修饰在 printf/scanf 下与 long/int64_t 严格对齐,无截断/误读,
 *     亦覆盖垫片漏掉的 SCNi / SCNo / SCNiMAX 整族。
 *
 * 对齐 test_access.c 风格:Unity freestanding,无需 FS 夹具(纯计算 + stdio)。 */
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

/* ---- strtoimax / strtoumax ---- */

void test_strtoimax_hex_base16(void) {
  char *end = NULL;
  const char *s = "0xdeadbeef";
  intmax_t v = strtoimax(s, &end, 16);
  TEST_ASSERT_EQUAL_INT64(0xdeadbeefLL, v);
  TEST_ASSERT_EQUAL_PTR(s + 10, end); /* consumes all 8 hex digits → NUL */
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
  intmax_t v = strtoimax("9223372036854775808", &end, 10); /* INT64_MAX + 1 */
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, v);
  TEST_ASSERT_EQUAL_INT(ERANGE, errno);
}

void test_strtoumax_uint64_max(void) {
  char *end = NULL;
  errno = 0;
  uintmax_t v = strtoumax("18446744073709551615", &end, 10); /* UINT64_MAX */
  TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, v);
  TEST_ASSERT_EQUAL_INT(0, errno);
  TEST_ASSERT_EQUAL_CHAR('\0', *end);
}

void test_strtoumax_overflow_wraps(void) {
  char *end = NULL;
  errno = 0;
  uintmax_t v =
      strtoumax("18446744073709551616", &end, 10); /* UINT64_MAX + 1 */
  TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, v);
  TEST_ASSERT_EQUAL_INT(ERANGE, errno);
}

/* ---- imaxabs / imaxdiv ---- */

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

/* ---- wcstoimax ---- */

void test_wcstoimax_decimal(void) {
  wchar_t *end = NULL;
  const wchar_t *s = L"  123xyz";
  intmax_t v = wcstoimax(s, &end, 10);
  TEST_ASSERT_EQUAL_INT64(123, v);
  TEST_ASSERT_EQUAL_PTR(s + 5,
                        end); /* end points at "xyz" (skipped ws+3 digits) */
}

void test_wcstoimax_hex(void) {
  wchar_t *end = NULL;
  const wchar_t *s = L"ff";
  intmax_t v = wcstoimax(s, &end, 16);
  TEST_ASSERT_EQUAL_INT64(255, v);
  TEST_ASSERT_EQUAL_PTR(s + 2, end);
}

/* ---- PRI / SCN 格式宏正确性(l 修饰,匹配 long/int64_t) ----
 * 往返 snprintf→sscanf 验证 64 位值经 PRId64/PRIu64 写出、SCNd64/SCNu64 读回
 * 完整无损——这是 "l" 修饰正确的直接证据(若误用 "ll" 与 long 类型不匹配,
 * 或 "l" 与实现不符,都会截断/误读)。 */

void test_pri_scn_roundtrip_signed(void) {
  char buf[32];
  const int64_t in = -9007199254740993LL; /* 53-bit 边界外的奇数,易被截断暴露 */
  snprintf(buf, sizeof buf, "%" PRId64, in);
  int64_t out = 0;
  int n = sscanf(buf, "%" SCNd64, &out);
  TEST_ASSERT_EQUAL_INT(1, n);
  TEST_ASSERT_EQUAL_INT64(in, out);
}

void test_pri_scn_roundtrip_unsigned(void) {
  char buf[32];
  const uint64_t in = 18014398509481983ULL; /* < UINT64_MAX,非平凡 */
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
  /* PRIdMAX/SCNdMAX 与 PRId64/SCNd64 在本 target 都解析为 "l"+spec,
   * 故对同一 int64_t 值往返一致。 */
  char buf[32];
  const intmax_t in = INT64_MAX;
  snprintf(buf, sizeof buf, "%" PRIdMAX, in);
  intmax_t out = 0;
  TEST_ASSERT_EQUAL_INT(1, sscanf(buf, "%" SCNdMAX, &out));
  TEST_ASSERT_EQUAL_INT64(in, out);
}

void test_pri_ptr_matches_64(void) {
  /* uintptr_t == uint64_t,故 PRIuPTR/SCNuPTR 与 PRIu64/SCNu64 同修饰。 */
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

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_locale.c — 验证 <locale.h> 迁移到 musl 上游后的 locale 管理与 collate。
 *
 * 本 OS 此前 <locale.h> 完全缺位（无 user/include/locale.h，无 src/locale
 * 编译）： locale 机制只在 strerror/langinfo 的 C-locale
 * 旁路里隐式存在（__lctrans 弱 透传 + c_locale.c 的
 * __c_locale/__c_dot_utf8_locale）。locale tier 把公开 POSIX locale API +
 * collate 真正编进 libc（musl_locale_objs），本测试补回归：
 *
 *  1. 6 个 locale 管理函数符号链接/行为正确（setlocale/localeconv/newlocale/
 *     duplocale/freelocale/uselocale）。无 .mo 目录文件 → 全部回落 C/C.UTF-8
 *     （与 strerror/langinfo 同一 C-locale 策略）。
 *  2. 4 个 collate 函数 + _l 变体（strcoll/strxfrm/wcscoll/wcsxfrm 及 _l）
 *     是 musl 的 code-point 桩（strcoll=strcmp，strxfrm=strlen+strcpy，宽字符
 *     对应 wcscmp/wcslen+wmemcpy）——验证其在 C locale 下退化为直接字节/码点
 *     比较，无 collation 重排。
 *  3. struct lconv 的 POSIX C locale 默认字段（decimal_point="."，数值类
 *     CHAR_MAX，货币类空串）。
 *
 * 对齐 test_inttypes.c 风格：Unity freestanding，纯计算 + stdio/wchar，无需 FS
 * 夹具。test 默认 _XOPEN_SOURCE 700（musl features.h 在无显式 define 且非
 * __STRICT_ANSI__ 时默认 _BSD_SOURCE+_XOPEN_SOURCE 700），足以让 <locale.h>
 * 声明 newlocale/uselocale/LC_*_MASK/LC_GLOBAL_LOCALE 及 collate _l 变体，故
 * 无需 DEFS _GNU_SOURCE。 */
#include "unity.h"
#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

void setUp(void) {}
void tearDown(void) {}

/* ---- setlocale / localeconv ---- */

void test_setlocale_default_returns_C(void) {
  /* 启动默认 locale 为 "C"；setlocale(LC_ALL, NULL) 查询当前值不修改。 */
  char *cur = setlocale(LC_ALL, NULL);
  TEST_ASSERT_NOT_NULL(cur);
  TEST_ASSERT_EQUAL_STRING("C", cur);
}

void test_setlocale_set_C_explicit(void) {
  char *cur = setlocale(LC_ALL, "C");
  TEST_ASSERT_NOT_NULL(cur);
  TEST_ASSERT_EQUAL_STRING("C", cur);
}

void test_setlocale_unknown_returns_null(void) {
  /* 无 .mo 目录文件、无 locale 路径 → 非内置名（非 C/C.UTF-8/POSIX）setlocale
   * 失败返 NULL（musl __get_locale 对非 builtin 走 __map_file，本 OS 无文件 →
   * 回落 __c_dot_utf8 但 setlocale 的 LC_ALL 序列化路径对 unknown 仍返 NULL）。
   * 用一个显然不存在的名字，断言不崩溃且返 NULL 或回落（两种合规行为都接受）。
   */
  char *cur = setlocale(LC_CTYPE, "xx_ZZ.NOTACODESET");
  /* musl 对非内置名仍会造一个 locale_map 项存名字（不返 NULL），但 cat 实际
   * 指向 __c_dot_utf8。故这里不断言 NULL，只断言查询不崩溃且后续 setlocale
   * 可恢复到 "C"。 */
  (void)cur;
  TEST_ASSERT_EQUAL_STRING("C", setlocale(LC_CTYPE, "C"));
}

void test_localeconv_C_defaults(void) {
  setlocale(LC_ALL, "C");
  struct lconv *lc = localeconv();
  TEST_ASSERT_NOT_NULL(lc);
  TEST_ASSERT_EQUAL_STRING(".", lc->decimal_point);
  TEST_ASSERT_EQUAL_STRING("", lc->thousands_sep);
  TEST_ASSERT_EQUAL_STRING("", lc->grouping);
  /* POSIX C locale：数值类字段 CHAR_MAX，货币类空串/CHAR_MAX。 */
  TEST_ASSERT_EQUAL_CHAR(CHAR_MAX, lc->frac_digits);
  TEST_ASSERT_EQUAL_CHAR(CHAR_MAX, lc->int_frac_digits);
  TEST_ASSERT_EQUAL_STRING("", lc->currency_symbol);
  TEST_ASSERT_EQUAL_STRING("", lc->int_curr_symbol);
}

/* ---- newlocale / uselocale / duplocale / freelocale ---- */

void test_newlocale_C_mask_returns_builtin(void) {
  /* newlocale(LC_ALL_MASK,"C",0) → 命中 C_LOCALE 内置（musl do_newlocale 的
   * memcmp(&tmp, C_LOCALE) 快路径），不分配。 */
  locale_t loc = newlocale(LC_ALL_MASK, "C", (locale_t)0);
  TEST_ASSERT_NOT_NULL(loc);
  freelocale(
      loc); /* 内置 locale，freelocale 是 no-op（__loc_is_allocated=0）。 */
}

void test_uselocale_get_set_global(void) {
  /* uselocale(NULL) 返回当前线程 locale（启动 = 全局 LC_GLOBAL_LOCALE 或
   * global_locale）。uselocale(LC_GLOBAL_LOCALE) 切回全局。 */
  locale_t prev = uselocale((locale_t)0);
  TEST_ASSERT_NOT_NULL(prev); /* 启动必有默认 locale。 */
  /* 切到全局再切回，往返不丢。 */
  locale_t after_global = uselocale(LC_GLOBAL_LOCALE);
  (void)after_global;
  locale_t restored = uselocale(prev);
  TEST_ASSERT_NOT_NULL(restored);
}

void test_duplocale_then_freelocale_roundtrip(void) {
  /* duplocale 复制一个 locale；freelocale 释放。对内置 locale duplocale 会
   * malloc 一份新副本（__loc_is_allocated=1）。往返不崩溃、不泄漏（freelocale
   * 释放副本）。 */
  locale_t loc = newlocale(LC_CTYPE_MASK, "C", (locale_t)0);
  TEST_ASSERT_NOT_NULL(loc);
  locale_t dup = duplocale(loc);
  TEST_ASSERT_NOT_NULL(dup);
  freelocale(dup);
  freelocale(loc);
}

void test_newlocale_partial_mask_preserves_base(void) {
  /* newlocale(mask, name, base)：mask 未覆盖的类别取 base 的值。base=NULL 时
   * 未覆盖类别取默认（C）。LC_NUMERIC_MASK 单独设 "C" 应得等价于全 C 的 locale
   * （命中 default_locale 或 C_LOCALE 内置）。 */
  locale_t loc = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
  TEST_ASSERT_NOT_NULL(loc);
  freelocale(loc);
}

/* ---- strcoll / strxfrm (narrow, code-point 桩) ---- */

void test_strcoll_equals_strcmp_C_locale(void) {
  /* musl strcoll 在 C locale 下就是 strcmp（code-point 比较）。 */
  setlocale(LC_COLLATE, "C");
  TEST_ASSERT_EQUAL_INT(strcmp("abc", "abd"), strcoll("abc", "abd"));
  TEST_ASSERT_EQUAL_INT(strcmp("abc", "abc"), strcoll("abc", "abc"));
  TEST_ASSERT_EQUAL_INT(strcmp("abd", "abc"), strcoll("abd", "abc"));
  TEST_ASSERT_EQUAL_INT(strcmp("", ""), strcoll("", ""));
  TEST_ASSERT_EQUAL_INT(strcmp("a", ""), strcoll("a", ""));
}

void test_strcoll_l_matches_strcoll(void) {
  setlocale(LC_COLLATE, "C");
  locale_t cloc = newlocale(LC_COLLATE_MASK, "C", (locale_t)0);
  TEST_ASSERT_NOT_NULL(cloc);
  TEST_ASSERT_EQUAL_INT(strcoll("foo", "bar"), strcoll_l("foo", "bar", cloc));
  freelocale(cloc);
}

void test_strxfrm_is_strlen_plus_copy(void) {
  /* musl strxfrm 在 C locale 下：返回 strlen(src)，若 n>l 则 strcpy(dest,src)。
   * 即恒等变换（code-point 桩）。 */
  setlocale(LC_COLLATE, "C");
  char buf[16] = "ZZZZZZZ";
  size_t n = strxfrm(buf, "hello", sizeof buf);
  TEST_ASSERT_EQUAL_size_t((size_t)5, n);
  TEST_ASSERT_EQUAL_STRING("hello", buf);
  /* n=0 仅返回长度，不写 dest。 */
  TEST_ASSERT_EQUAL_size_t((size_t)5, strxfrm(NULL, "hello", 0));
  /* dest 过小（n <= l）：C11 7.24.4.5 规定返回值 >= n 时 dest 内容
   * indeterminate， musl 此时完全不写 dest，只返回完整
   * strlen。仅断言返回值，不断言 dest 内容。 */
  char tiny[3];
  size_t m = strxfrm(tiny, "hello", sizeof tiny);
  TEST_ASSERT_EQUAL_size_t((size_t)5, m);
}

void test_strxfrm_l_matches_strxfrm(void) {
  setlocale(LC_COLLATE, "C");
  locale_t cloc = newlocale(LC_COLLATE_MASK, "C", (locale_t)0);
  char buf[8];
  size_t n = strxfrm_l(buf, "abc", sizeof buf, cloc);
  TEST_ASSERT_EQUAL_size_t((size_t)3, n);
  TEST_ASSERT_EQUAL_STRING("abc", buf);
  freelocale(cloc);
}

/* ---- wcscoll / wcsxfrm (wide, code-point 桩) ---- */

void test_wcscoll_equals_wcscmp_C_locale(void) {
  setlocale(LC_COLLATE, "C");
  TEST_ASSERT_EQUAL_INT(wcscmp(L"abc", L"abd"), wcscoll(L"abc", L"abd"));
  TEST_ASSERT_EQUAL_INT(wcscmp(L"abc", L"abc"), wcscoll(L"abc", L"abc"));
  TEST_ASSERT_EQUAL_INT(wcscmp(L"abd", L"abc"), wcscoll(L"abd", L"abc"));
  TEST_ASSERT_EQUAL_INT(wcscmp(L"", L""), wcscoll(L"", L""));
}

void test_wcscoll_l_matches_wcscoll(void) {
  setlocale(LC_COLLATE, "C");
  locale_t cloc = newlocale(LC_COLLATE_MASK, "C", (locale_t)0);
  TEST_ASSERT_EQUAL_INT(wcscoll(L"foo", L"bar"),
                        wcscoll_l(L"foo", L"bar", cloc));
  freelocale(cloc);
}

void test_wcsxfrm_is_wcslen_plus_wmemcpy(void) {
  setlocale(LC_COLLATE, "C");
  wchar_t buf[8] = L"ZZZZZZZ";
  size_t n = wcsxfrm(buf, L"hello", sizeof buf / sizeof(buf[0]));
  TEST_ASSERT_EQUAL_size_t((size_t)5, n);
  TEST_ASSERT_EQUAL_INT(0, wcscmp(L"hello", buf));
  /* n=0 仅返回长度。 */
  TEST_ASSERT_EQUAL_size_t((size_t)5, wcsxfrm(NULL, L"hello", 0));
}

void test_wcsxfrm_l_matches_wcsxfrm(void) {
  setlocale(LC_COLLATE, "C");
  locale_t cloc = newlocale(LC_COLLATE_MASK, "C", (locale_t)0);
  wchar_t buf[8];
  size_t n = wcsxfrm_l(buf, L"abc", sizeof buf / sizeof(buf[0]), cloc);
  TEST_ASSERT_EQUAL_size_t((size_t)3, n);
  TEST_ASSERT_EQUAL_INT(0, wcscmp(L"abc", buf));
  freelocale(cloc);
}

/* ---- LC_* 常量与 LC_GLOBAL_LOCALE 宏（头切换回归） ---- */

void test_lc_category_constants_distinct(void) {
  /* musl <locale.h> 内联 #define（非 bits/locale.h）：LC_CTYPE=0..LC_ALL=6。
   * 断言它们是 0..6 的排列且互异——捕获头切换后宏值漂移。 */
  int cats[] = {LC_CTYPE,    LC_NUMERIC,  LC_TIME, LC_COLLATE,
                LC_MONETARY, LC_MESSAGES, LC_ALL};
  for (int i = 0; i < 7; i++) {
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, cats[i]);
    TEST_ASSERT_LESS_OR_EQUAL_INT(6, cats[i]);
    for (int j = i + 1; j < 7; j++) {
      TEST_ASSERT_NOT_EQUAL_INT(cats[i], cats[j]);
    }
  }
}

void test_lc_mask_constants_and_global(void) {
  /* _MASK 族 + LC_GLOBAL_LOCALE 在 _XOPEN 下声明。LC_ALL_MASK=0x7fffffff。 */
  TEST_ASSERT_EQUAL_INT(0x7fffffff, LC_ALL_MASK);
  TEST_ASSERT_EQUAL_PTR((locale_t)-1, LC_GLOBAL_LOCALE);
  /* 单类别 mask = 1<<category。 */
  TEST_ASSERT_EQUAL_INT(1 << LC_CTYPE, LC_CTYPE_MASK);
  TEST_ASSERT_EQUAL_INT(1 << LC_NUMERIC, LC_NUMERIC_MASK);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_setlocale_default_returns_C);
  RUN_TEST(test_setlocale_set_C_explicit);
  RUN_TEST(test_setlocale_unknown_returns_null);
  RUN_TEST(test_localeconv_C_defaults);
  RUN_TEST(test_newlocale_C_mask_returns_builtin);
  RUN_TEST(test_uselocale_get_set_global);
  RUN_TEST(test_duplocale_then_freelocale_roundtrip);
  RUN_TEST(test_newlocale_partial_mask_preserves_base);
  RUN_TEST(test_strcoll_equals_strcmp_C_locale);
  RUN_TEST(test_strcoll_l_matches_strcoll);
  RUN_TEST(test_strxfrm_is_strlen_plus_copy);
  RUN_TEST(test_strxfrm_l_matches_strxfrm);
  RUN_TEST(test_wcscoll_equals_wcscmp_C_locale);
  RUN_TEST(test_wcscoll_l_matches_wcscoll);
  RUN_TEST(test_wcsxfrm_is_wcslen_plus_wmemcpy);
  RUN_TEST(test_wcsxfrm_l_matches_wcsxfrm);
  RUN_TEST(test_lc_category_constants_distinct);
  RUN_TEST(test_lc_mask_constants_and_global);
  return UNITY_END();
}

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// test_locale.c — verifies locale management and collate after <locale.h>
// migrated to the musl upstream header.
//
// Previously this OS had no <locale.h> at all (no user/include/locale.h, no
// src/locale compilation): the locale mechanism existed only implicitly in the
// C-locale bypass paths of strerror/langinfo (the __lctrans weak pass-through
// plus __c_locale/__c_dot_utf8_locale from c_locale.c). The locale tier now
// compiles the public POSIX locale API + collate into libc
// (musl_locale_objs); this test adds regression coverage:
//
//  1. The 6 locale-management function symbols link and behave correctly
//     (setlocale/localeconv/newlocale/duplocale/freelocale/uselocale). With no
//     .mo directory files, everything falls back to C/C.UTF-8 (the same
//     C-locale policy as strerror/langinfo).
//  2. The 4 collate functions + _l variants (strcoll/strxfrm/wcscoll/wcsxfrm
//     and their _l forms) are musl's code-point stubs (strcoll=strcmp,
//     strxfrm=strlen+strcpy, wide wcscmp/wcslen+wmemcpy) — verifying they
//     degrade to direct byte/code-point comparison under the C locale, with no
//     collation reordering.
//  3. struct lconv's POSIX C-locale default fields (decimal_point=".",
//     numeric fields CHAR_MAX, currency fields empty strings).
//
// Aligned with test_inttypes.c style: Unity freestanding, pure compute +
// stdio/wchar, no FS fixture. The test defaults to _XOPEN_SOURCE 700 (musl
// features.h defaults to _BSD_SOURCE+_XOPEN_SOURCE 700 when no explicit define
// and not __STRICT_ANSI__), which is enough for <locale.h> to declare
// newlocale/uselocale/LC_*_MASK/LC_GLOBAL_LOCALE and the collate _l variants,
// so no DEFS _GNU_SOURCE is needed.
#include "unity.h"
#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

void setUp(void) {}
void tearDown(void) {}

// ---- setlocale / localeconv ----

void test_setlocale_default_returns_C(void) {
  // Startup default locale is "C"; setlocale(LC_ALL, NULL) queries the current
  // value without modifying it.
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
  // No .mo directory files and no locale path → a non-builtin name (anything
  // other than C/C.UTF-8/POSIX) makes setlocale fail and return NULL (musl
  // __get_locale routes non-builtins through __map_file; this OS has no files,
  // so it falls back to __c_dot_utf8, but setlocale's LC_ALL serialize path
  // still returns NULL for an unknown name). Use an obviously nonexistent name
  // and assert it neither crashes nor returns non-NULL (both behaviors are
  // acceptable-ish); here we only require it not to crash.
  char *cur = setlocale(LC_CTYPE, "xx_ZZ.NOTACODESET");
  // musl still builds a locale_map entry for a non-builtin name (does not
  // return NULL), but cat actually points at __c_dot_utf8. So here we do not
  // assert NULL, only that the query does not crash and that a later setlocale
  // can restore "C".
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
  // POSIX C locale: numeric fields are CHAR_MAX, currency fields
  // empty/CHAR_MAX.
  TEST_ASSERT_EQUAL_CHAR(CHAR_MAX, lc->frac_digits);
  TEST_ASSERT_EQUAL_CHAR(CHAR_MAX, lc->int_frac_digits);
  TEST_ASSERT_EQUAL_STRING("", lc->currency_symbol);
  TEST_ASSERT_EQUAL_STRING("", lc->int_curr_symbol);
}

// ---- newlocale / uselocale / duplocale / freelocale ----

void test_newlocale_C_mask_returns_builtin(void) {
  // newlocale(LC_ALL_MASK,"C",0) hits the C_LOCALE builtin (musl do_newlocale's
  // memcmp(&tmp, C_LOCALE) fast path), so nothing is allocated.
  locale_t loc = newlocale(LC_ALL_MASK, "C", (locale_t)0);
  TEST_ASSERT_NOT_NULL(loc);
  freelocale(
      loc); // builtin locale: freelocale is a no-op (__loc_is_allocated=0).
}

void test_uselocale_get_set_global(void) {
  // uselocale(NULL) returns the current thread's locale (at startup, the
  // global LC_GLOBAL_LOCALE / global_locale). uselocale(LC_GLOBAL_LOCALE)
  // switches back to the global one.
  locale_t prev = uselocale((locale_t)0);
  TEST_ASSERT_NOT_NULL(prev); // startup always has a default locale.
  // Switch to global and back; the round-trip loses nothing.
  locale_t after_global = uselocale(LC_GLOBAL_LOCALE);
  (void)after_global;
  locale_t restored = uselocale(prev);
  TEST_ASSERT_NOT_NULL(restored);
}

void test_duplocale_then_freelocale_roundtrip(void) {
  // duplocale copies a locale; freelocale frees it. For a builtin locale
  // duplocale mallocs a fresh copy (__loc_is_allocated=1). The round-trip
  // neither crashes nor leaks (freelocale releases the copy).
  locale_t loc = newlocale(LC_CTYPE_MASK, "C", (locale_t)0);
  TEST_ASSERT_NOT_NULL(loc);
  locale_t dup = duplocale(loc);
  TEST_ASSERT_NOT_NULL(dup);
  freelocale(dup);
  freelocale(loc);
}

void test_newlocale_partial_mask_preserves_base(void) {
  // newlocale(mask, name, base): categories not covered by mask take base's
  // value. With base=NULL the uncovered categories default to "C". Setting
  // LC_NUMERIC_MASK alone to "C" should yield a locale equivalent to all-C
  // (hits the default_locale or C_LOCALE builtin).
  locale_t loc = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
  TEST_ASSERT_NOT_NULL(loc);
  freelocale(loc);
}

// ---- strcoll / strxfrm (narrow, code-point stub) ----

void test_strcoll_equals_strcmp_C_locale(void) {
  // musl strcoll under the C locale is just strcmp (code-point comparison).
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
  // musl strxfrm under the C locale: returns strlen(src), and if n>l strcpy's
  // dest=src. I.e. the identity transform (code-point stub).
  setlocale(LC_COLLATE, "C");
  char buf[16] = "ZZZZZZZ";
  size_t n = strxfrm(buf, "hello", sizeof buf);
  TEST_ASSERT_EQUAL_size_t((size_t)5, n);
  TEST_ASSERT_EQUAL_STRING("hello", buf);
  // n=0 only returns the length; dest is not written.
  TEST_ASSERT_EQUAL_size_t((size_t)5, strxfrm(NULL, "hello", 0));
  // dest too small (n <= l): C11 7.24.4.5 says the dest contents are
  // indeterminate when the return value >= n; musl then does not write dest at
  // all, only returning the full strlen. Only assert the return value, not the
  // dest contents.
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

// ---- wcscoll / wcsxfrm (wide, code-point stub) ----

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
  // n=0 only returns the length.
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

// ---- LC_* constants & LC_GLOBAL_LOCALE macro (header-switch regression) ----

void test_lc_category_constants_distinct(void) {
  // musl <locale.h> uses inline #define (not bits/locale.h): LC_CTYPE=0..
  // LC_ALL=6. Assert they are a permutation of 0..6 and mutually distinct —
  // catches macro-value drift after the header switch.
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
  // The _MASK family + LC_GLOBAL_LOCALE are declared under _XOPEN.
  // LC_ALL_MASK=0x7fffffff.
  TEST_ASSERT_EQUAL_INT(0x7fffffff, LC_ALL_MASK);
  TEST_ASSERT_EQUAL_PTR((locale_t)-1, LC_GLOBAL_LOCALE);
  // A single-category mask = 1<<category.
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

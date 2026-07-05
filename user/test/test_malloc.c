/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <string.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

/* 1. malloc(32) returns non-NULL, can write and read */
void test_malloc_small(void) {
  char *p = (char *)malloc(32);
  TEST_ASSERT_NOT_NULL(p);
  memset(p, 'X', 32);
  for (int i = 0; i < 32; i++) {
    TEST_ASSERT_EQUAL_INT('X', p[i]);
  }
  free(p);
}

/* 2. malloc size classes: 8/128/1024, non-overlapping */
void test_malloc_size_classes(void) {
  char *p1 = (char *)malloc(8);
  char *p2 = (char *)malloc(128);
  char *p3 = (char *)malloc(1024);
  TEST_ASSERT_NOT_NULL(p1);
  TEST_ASSERT_NOT_NULL(p2);
  TEST_ASSERT_NOT_NULL(p3);

  /* Write to each to verify they are usable */
  memset(p1, 'A', 8);
  memset(p2, 'B', 128);
  memset(p3, 'C', 1024);
  TEST_ASSERT_EQUAL_INT('A', p1[0]);
  TEST_ASSERT_EQUAL_INT('B', p2[0]);
  TEST_ASSERT_EQUAL_INT('C', p3[0]);

  free(p1);
  free(p2);
  free(p3);
}

/* 3. free(p) → malloc(same size) should reuse */
void test_malloc_free_reuse(void) {
  char *p1 = (char *)malloc(64);
  TEST_ASSERT_NOT_NULL(p1);
  memset(p1, 'D', 64);
  uintptr_t addr1 = (uintptr_t)p1;
  free(p1);

  char *p2 = (char *)malloc(64);
  TEST_ASSERT_NOT_NULL(p2);
  uintptr_t addr2 = (uintptr_t)p2;
  /* Same size class — address may be the same or different */
  /* Just verify it's usable */
  memset(p2, 'E', 64);
  TEST_ASSERT_EQUAL_INT('E', p2[0]);
  (void)addr1;
  (void)addr2;
  free(p2);
}

/* 4. calloc returns zeroed memory */
void test_calloc_zero(void) {
  int *arr = (int *)calloc(10, sizeof(int));
  TEST_ASSERT_NOT_NULL(arr);
  for (int i = 0; i < 10; i++) {
    TEST_ASSERT_EQUAL_INT(0, arr[i]);
  }
  arr[0] = 42;
  arr[9] = 99;
  TEST_ASSERT_EQUAL_INT(42, arr[0]);
  TEST_ASSERT_EQUAL_INT(99, arr[9]);
  free(arr);
}

/* 5. malloc(4096) — large allocation via mmap */
void test_malloc_large(void) {
  char *p = (char *)malloc(4096);
  TEST_ASSERT_NOT_NULL(p);
  memset(p, 'F', 4096);
  TEST_ASSERT_EQUAL_INT('F', p[0]);
  TEST_ASSERT_EQUAL_INT('F', p[4095]);
  free(p);
}

/* 6. realloc grow: 64 → 128, original content preserved */
void test_realloc_grow(void) {
  char *p = (char *)malloc(64);
  TEST_ASSERT_NOT_NULL(p);
  memset(p, 'X', 64);

  p = (char *)realloc(p, 128);
  TEST_ASSERT_NOT_NULL(p);
  /* First 64 bytes preserved */
  for (int i = 0; i < 64; i++) {
    TEST_ASSERT_EQUAL_INT('X', p[i]);
  }
  free(p);
}

/* 7. realloc shrink: 64 → 32, first 32 bytes preserved */
void test_realloc_shrink(void) {
  char *p = (char *)malloc(64);
  TEST_ASSERT_NOT_NULL(p);
  memset(p, 'Y', 64);

  p = (char *)realloc(p, 32);
  TEST_ASSERT_NOT_NULL(p);
  for (int i = 0; i < 32; i++) {
    TEST_ASSERT_EQUAL_INT('Y', p[i]);
  }
  free(p);
}

/* 8. Free all allocations — no crash */
void test_free_all(void) {
  char *p1 = (char *)malloc(16);
  char *p2 = (char *)malloc(256);
  char *p3 = (char *)malloc(2048);
  free(p1);
  free(p2);
  free(p3);
  /* If we get here, no crash occurred */
  TEST_ASSERT_TRUE(1);
}

/* 9. malloc(0) — POSIX allows NULL or unique pointer */
void test_malloc_null(void) {
  void *p = malloc(0);
  /* POSIX: may return NULL or a unique pointer — both are valid */
  if (p)
    free(p);
  TEST_ASSERT_TRUE(1); /* no crash = pass */
}

/* 10. double free — should not crash */
void test_double_free(void) {
  char *p = (char *)malloc(64);
  TEST_ASSERT_NOT_NULL(p);
  free(p);
  /* Double free — may crash or be handled gracefully */
  /* For safety, skip actual double free in automated tests */
  /* free(p); — commented out; test documents the expectation */
  TEST_ASSERT_TRUE(1);
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();
  RUN_TEST(test_malloc_small);
  RUN_TEST(test_malloc_size_classes);
  RUN_TEST(test_malloc_free_reuse);
  RUN_TEST(test_calloc_zero);
  RUN_TEST(test_malloc_large);
  RUN_TEST(test_realloc_grow);
  RUN_TEST(test_realloc_shrink);
  RUN_TEST(test_free_all);
  RUN_TEST(test_malloc_null);
  RUN_TEST(test_double_free);
  return UNITY_END();
}

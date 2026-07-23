/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// S13: mmap flag handling — recognized-but-unsupported flags are no-op,
// genuinely-unknown bits are rejected with -EINVAL, and MAP_LOCKED now uses
// the Linux value 0x2000 (0x100 belongs to MAP_GROWSDOWN).

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <unity.h>

#define PAGE 4096UL

void setUp(void) {}
void tearDown(void) {}

static void *anon_with_flag(int extra) {
  return mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
              MAP_PRIVATE | MAP_ANONYMOUS | extra, -1, 0);
}

/* TC1: MAP_LOCKED is accepted (no swap here → no-op). */
void test_mmap_locked_noop(void) {
  void *p = anon_with_flag(MAP_LOCKED);
  TEST_ASSERT_NOT_NULL(p);
  *(char *)p = 'x';
  TEST_ASSERT_EQUAL_INT('x', *(char *)p);
  munmap(p, PAGE);
}

/* TC2: MAP_HUGETLB is accepted as no-op (ordinary 4KB page). */
void test_mmap_hugetlb_noop(void) {
  void *p = anon_with_flag(MAP_HUGETLB);
  TEST_ASSERT_NOT_NULL(p);
  *(char *)p = 'x';
  TEST_ASSERT_EQUAL_INT('x', *(char *)p);
  munmap(p, PAGE);
}

/* TC3: MAP_POPULATE is a no-op (anonymous mmap pre-allocates already). */
void test_mmap_populate_noop(void) {
  void *p = anon_with_flag(MAP_POPULATE);
  TEST_ASSERT_NOT_NULL(p);
  *(char *)p = 'x';
  TEST_ASSERT_EQUAL_INT('x', *(char *)p);
  munmap(p, PAGE);
}

/* TC4: MAP_STACK is a no-op (fixed user stack, no special placement). */
void test_mmap_stack_noop(void) {
  void *p = anon_with_flag(MAP_STACK);
  TEST_ASSERT_NOT_NULL(p);
  *(char *)p = 'x';
  TEST_ASSERT_EQUAL_INT('x', *(char *)p);
  munmap(p, PAGE);
}

/* TC5: MAP_NORESERVE is a no-op (no swap to reserve). */
void test_mmap_noreserve_noop(void) {
  void *p = anon_with_flag(MAP_NORESERVE);
  TEST_ASSERT_NOT_NULL(p);
  *(char *)p = 'x';
  TEST_ASSERT_EQUAL_INT('x', *(char *)p);
  munmap(p, PAGE);
}

/* TC6: MAP_GROWSDOWN is a no-op (no stack-grow mechanism). */
void test_mmap_growsdown_noop(void) {
  void *p = anon_with_flag(MAP_GROWSDOWN);
  TEST_ASSERT_NOT_NULL(p);
  *(char *)p = 'x';
  TEST_ASSERT_EQUAL_INT('x', *(char *)p);
  munmap(p, PAGE);
}

/* TC7: a genuinely-unknown flag bit is rejected with -EINVAL.
 * 0x40000000 is not in MAP_KNOWN_FLAGS (0x80000000 is MAP_PHYSICAL, which
 * *is* recognized). */
void test_mmap_unknown_flag_einval(void) {
  void *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | 0x40000000, -1, 0);
  TEST_ASSERT_EQUAL_PTR(MAP_FAILED, p);
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();
  RUN_TEST(test_mmap_locked_noop);
  RUN_TEST(test_mmap_hugetlb_noop);
  RUN_TEST(test_mmap_populate_noop);
  RUN_TEST(test_mmap_stack_noop);
  RUN_TEST(test_mmap_noreserve_noop);
  RUN_TEST(test_mmap_growsdown_noop);
  RUN_TEST(test_mmap_unknown_flag_einval);
  return UNITY_END();
}

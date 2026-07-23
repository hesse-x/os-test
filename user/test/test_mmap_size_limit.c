/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// S13: mmap size limit removal.
//
// Before S13 sys_mmap rejected any size > 128 MiB with -EINVAL. The cap is
// gone; the natural limit is now bfc_alloc_page / address-space failure →
// -ENOMEM (RLIMIT_AS is a future todo; the OS has no rlimit framework). So a
// large anonymous mmap may legitimately return either a valid address (if the
// free pool can satisfy it) or -ENOMEM, but must NOT return -EINVAL.

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <unity.h>

#define PAGE 4096UL

void setUp(void) {}
void tearDown(void) {}

/* TC1: a >128 MiB anonymous mmap no longer hits the old -EINVAL cap. It
 * succeeds if the free pool is healthy, or returns -ENOMEM if not — both are
 * acceptable. Only -EINVAL would indicate the old hardcap is still in place. */
void test_mmap_large_not_einval(void) {
  size_t big = 200 * 1024 * 1024;
  void *p = mmap(NULL, big, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                 -1, 0);
  if (p != MAP_FAILED) {
    // Got the pages — touch one and release.
    *(char *)p = 'x';
    TEST_ASSERT_EQUAL_INT('x', *(char *)p);
    munmap(p, big);
  } else {
    TEST_ASSERT_EQUAL_INT(ENOMEM, errno);
  }
}

/* TC2: size==0 anonymous mmap is still -EINVAL (MAP_SHARED && fd>=0 is the
 * only size==0 exception Linux allows). */
void test_mmap_size_zero_einval(void) {
  void *p =
      mmap(NULL, 0, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  TEST_ASSERT_EQUAL_PTR(MAP_FAILED, p);
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();
  RUN_TEST(test_mmap_large_not_einval);
  RUN_TEST(test_mmap_size_zero_einval);
  return UNITY_END();
}

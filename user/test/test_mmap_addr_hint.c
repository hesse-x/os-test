/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// S11: mmap addr hint + MAP_FIXED / MAP_FIXED_NOREPLACE.
//
// Covers the Linux addr-placement semantics introduced in S11:
//   - non-MAP_FIXED addr is a hint (satisfied when free, bumped when busy)
//   - MAP_FIXED places exactly at addr, over-unmapping existing mappings
//   - MAP_FIXED_NOREPLACE fails with -EEXIST on any overlap, leaving the
//     existing mapping untouched
//   - MAP_FIXED requires a page-aligned addr (else -EINVAL)
//   - mmap_brk only advances on the bump path, not on MAP_FIXED placement
//
// Hint/fixed addresses are chosen above the mmap_brk bump zone (starts at
// 0x800000) and below the user stack (0x7FFFFFE000), so they land in free
// space and don't collide with heap allocations made by Unity itself.
// NOTE: must avoid SIG_TRAMPOLINE_ADDR (0x50000000) — the kernel maps the
// signal trampoline page there in every process, so a MAP_FIXED request at
// 0x50000000 is correctly rejected (it would corrupt signal delivery).

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

#define PAGE 4096UL
#define HINT_A 0x40000000UL
#define HINT_B 0x48000000UL
#define HINT_C 0x60000000UL
#define HINT_D 0x70000000UL

/* TC1: hint with no conflict is honored. */
void test_hint_satisfied(void) {
  void *p = mmap((void *)HINT_A, PAGE, PROT_READ | PROT_WRITE,
                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQUAL_PTR((void *)HINT_A, p);
  *(volatile int *)p = 1;
  TEST_ASSERT_EQUAL_INT(1, *(volatile int *)p);
  munmap(p, PAGE);
}

/* TC2: conflicting hint falls back to a different (bump) address. */
void test_hint_conflict_fallback(void) {
  void *p1 = mmap((void *)HINT_A, PAGE, PROT_READ | PROT_WRITE,
                  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  TEST_ASSERT_EQUAL_PTR((void *)HINT_A, p1);
  *(volatile int *)p1 = 11;

  void *p2 = mmap((void *)HINT_A, PAGE, PROT_READ | PROT_WRITE,
                  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  TEST_ASSERT_NOT_NULL(p2);
  TEST_ASSERT_NOT_EQUAL((void *)HINT_A, p2);
  *(volatile int *)p2 = 22;
  TEST_ASSERT_EQUAL_INT(11, *(volatile int *)p1);
  TEST_ASSERT_EQUAL_INT(22, *(volatile int *)p2);

  munmap(p1, PAGE);
  munmap(p2, PAGE);
}

/* TC3: a non-page-aligned hint is tolerated (rounded down or bumped). */
void test_hint_unaligned(void) {
  void *p = mmap((void *)(HINT_A + 0x500), PAGE, PROT_READ | PROT_WRITE,
                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_TRUE(((uintptr_t)p & (PAGE - 1)) == 0); // result always aligned
  *(volatile int *)p = 3;
  TEST_ASSERT_EQUAL_INT(3, *(volatile int *)p);
  munmap(p, PAGE);
}

/* TC4: MAP_FIXED places exactly at the requested page-aligned addr. */
void test_fixed_basic(void) {
  void *p = mmap((void *)HINT_B, PAGE, PROT_READ | PROT_WRITE,
                 MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  TEST_ASSERT_EQUAL_PTR((void *)HINT_B, p);
  *(volatile int *)p = 4;
  TEST_ASSERT_EQUAL_INT(4, *(volatile int *)p);
  munmap(p, PAGE);
}

/* TC5: MAP_FIXED overwrites part of an existing mapping; the untouched tail
 * keeps its data, the overwritten head reads back zero (fresh page). */
void test_fixed_overwrite_partial(void) {
  void *p = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE,
                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  TEST_ASSERT_NOT_NULL(p);
  *(volatile int *)p = 42;
  *(volatile int *)((char *)p + PAGE) = 99;

  void *q = mmap(p, PAGE, PROT_READ | PROT_WRITE,
                 MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  TEST_ASSERT_EQUAL_PTR(p, q);
  TEST_ASSERT_EQUAL_INT(0, *(volatile int *)p); // overwritten → zero
  TEST_ASSERT_EQUAL_INT(99, *(volatile int *)((char *)p + PAGE)); // tail kept

  munmap(p, 2 * PAGE);
}

/* TC6: MAP_FIXED with an unaligned addr fails with EINVAL. */
void test_fixed_unaligned_einval(void) {
  errno = 0;
  void *p = mmap((void *)(HINT_B + 0x500), PAGE, PROT_READ | PROT_WRITE,
                 MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  TEST_ASSERT_EQUAL_PTR(MAP_FAILED, p);
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
}

/* TC7: MAP_FIXED_NOREPLACE with no conflict places exactly at addr. */
void test_noreplace_no_conflict(void) {
  void *p = mmap((void *)HINT_C, PAGE, PROT_READ | PROT_WRITE,
                 MAP_FIXED_NOREPLACE | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  TEST_ASSERT_EQUAL_PTR((void *)HINT_C, p);
  *(volatile int *)p = 7;
  TEST_ASSERT_EQUAL_INT(7, *(volatile int *)p);
  munmap(p, PAGE);
}

/* TC8: MAP_FIXED_NOREPLACE on an existing mapping fails with EEXIST and does
 * not disturb the existing data. */
void test_noreplace_conflict_eexist(void) {
  void *p = mmap((void *)HINT_C, PAGE, PROT_READ | PROT_WRITE,
                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  TEST_ASSERT_EQUAL_PTR((void *)HINT_C, p);
  *(volatile int *)p = 88;

  errno = 0;
  void *q = mmap((void *)HINT_C, PAGE, PROT_READ | PROT_WRITE,
                 MAP_FIXED_NOREPLACE | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  TEST_ASSERT_EQUAL_PTR(MAP_FAILED, q);
  TEST_ASSERT_EQUAL_INT(EEXIST, errno);
  TEST_ASSERT_EQUAL_INT(88, *(volatile int *)p); // existing untouched

  munmap(p, PAGE);
}

/* TC9: MAP_FIXED overwriting a SHM mapping releases that mapping instance
 * (shm_put) but the shm object survives while the memfd stays open; remapping
 * SHM over the same range reads back the original data. */
void test_fixed_overwrite_shm(void) {
  int fd = memfd_create("s11_shm", 0);
  TEST_ASSERT_TRUE(fd >= 0);
  TEST_ASSERT_EQUAL_INT(0, ftruncate(fd, 2 * PAGE));

  char *shm =
      (char *)mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  TEST_ASSERT_NOT_NULL(shm);
  *(volatile int *)shm = 0x1234;
  *(volatile int *)(shm + PAGE) = 0x5678;

  // Overwrite the first page with an anonymous fixed mapping.
  void *anon = mmap(shm, PAGE, PROT_READ | PROT_WRITE,
                    MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  TEST_ASSERT_EQUAL_PTR(shm, anon);
  TEST_ASSERT_EQUAL_INT(0, *(volatile int *)shm); // fresh zero page

  // Remap SHM back over the whole range; original data must still be present
  // (the shm object outlives the overwritten instance while the fd is open).
  // MAP_FIXED is required: step (1) above left the SHM tail page mapped, so a
  // bare hint would collide and the kernel would place the remap elsewhere.
  void *remap = mmap(shm, 2 * PAGE, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_FIXED, fd, 0);
  TEST_ASSERT_EQUAL_PTR(shm, remap);
  TEST_ASSERT_EQUAL_INT(0x1234, *(volatile int *)shm);
  TEST_ASSERT_EQUAL_INT(0x5678, *(volatile int *)(shm + PAGE));

  munmap(shm, 2 * PAGE);
  close(fd);
}

/* TC11: mmap_brk does not advance on a MAP_FIXED placement. Two NULL mmaps
 * bump consecutively; a fixed mapping elsewhere; the next NULL mmap continues
 * right after the second bump (not skipping past the fixed mapping). */
void test_brk_no_advance_on_fixed(void) {
  void *v0 = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  TEST_ASSERT_NOT_NULL(v0);
  void *v1 = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  TEST_ASSERT_NOT_NULL(v1);
  TEST_ASSERT_EQUAL_PTR((void *)((char *)v0 + PAGE), v1);

  void *fx = mmap((void *)HINT_D, PAGE, PROT_READ | PROT_WRITE,
                  MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  TEST_ASSERT_EQUAL_PTR((void *)HINT_D, fx);

  void *v2 = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  TEST_ASSERT_NOT_NULL(v2);
  TEST_ASSERT_EQUAL_PTR((void *)((char *)v1 + PAGE), v2);

  munmap(v0, PAGE);
  munmap(v1, PAGE);
  munmap(fx, PAGE);
  munmap(v2, PAGE);
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();
  RUN_TEST(test_hint_satisfied);
  RUN_TEST(test_hint_conflict_fallback);
  RUN_TEST(test_hint_unaligned);
  RUN_TEST(test_fixed_basic);
  RUN_TEST(test_fixed_overwrite_partial);
  RUN_TEST(test_fixed_unaligned_einval);
  RUN_TEST(test_noreplace_no_conflict);
  RUN_TEST(test_noreplace_conflict_eexist);
  RUN_TEST(test_fixed_overwrite_shm);
  RUN_TEST(test_brk_no_advance_on_fixed);
  return UNITY_END();
}

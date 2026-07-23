/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// S13: munmap partial / hole-punch interval semantics.
//
// Before S13 sys_munmap only dropped a region whose start exactly matched
// addr (size was ignored, sub-interval unmaps returned -EINVAL). This file
// exercises the new interval behavior:
//   - full-region unmap
//   - middle hole-punch (front/tail residue survive)
//   - tail truncation / head truncation
//   - cross-region unmap
//   - empty interval is a silent no-op success
//   - unaligned addr / size alignment rules
//   - SHM hole-punch keeps the object alive (refcount balance) and its data
//
// Faulting cases run in forked children with an SA_SIGINFO handler so the
// child captures si_code before terminating; the parent reaps WEXITSTATUS.

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/process.h>
#include <sys/wait.h>
#include <unistd.h>

#include <unity.h>

#ifndef PROT_NONE
#define PROT_NONE 0
#endif

#define PAGE 4096UL

void setUp(void) {}
void tearDown(void) {}

static volatile void *g_fault_page;

static void segv_handler(int sig, siginfo_t *info, void *uctx) {
  (void)sig;
  (void)uctx;
  void *addr = info->_sifields.si_addr;
  if (!((uintptr_t)addr >= (uintptr_t)g_fault_page &&
        (uintptr_t)addr < (uintptr_t)g_fault_page + PAGE))
    _exit(0xfe);
  _exit(info->si_code); // SEGV_MAPERR(1) for an unmapped page
}

static void install_segv_capture(void) {
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_sigaction = segv_handler;
  act.sa_flags = SA_SIGINFO;
  sigemptyset(&act.sa_mask);
  sigaction(SIGSEGV, &act, NULL);
}

// Returns the si_code the child captured (0 if it was killed by SIGSEGV
// without running the handler).
static int reap_child_code(pid_t pid) {
  int status;
  waitpid(pid, &status, 0);
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  return 0;
}

// Expect accessing fault_page in a child to fault with SEGV_MAPERR (page is
// no longer mapped after munmap).
static void expect_unmapped(void *fault_page) {
  g_fault_page = fault_page;
  pid_t pid = fork();
  if (pid == 0) {
    install_segv_capture();
    *(volatile char *)fault_page = 'x';
    _exit(0);
  }
  TEST_ASSERT_EQUAL_INT(SEGV_MAPERR, reap_child_code(pid));
}

/* TC1: full-region unmap. */
void test_munmap_full(void) {
  char *p = mmap(NULL, 3 * PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  TEST_ASSERT_NOT_NULL(p);
  p[0] = 'A';
  TEST_ASSERT_EQUAL_INT(0, munmap(p, 3 * PAGE));
  expect_unmapped(p);
}

/* TC2: middle hole-punch — front and tail residue survive. */
void test_munmap_hole_middle(void) {
  char *p = mmap(NULL, 3 * PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  TEST_ASSERT_NOT_NULL(p);
  p[0] = 'a';
  p[PAGE] = 'b';
  p[2 * PAGE] = 'c';
  TEST_ASSERT_EQUAL_INT(0, munmap(p + PAGE, PAGE));
  expect_unmapped(p + PAGE);
  TEST_ASSERT_EQUAL_INT('a', p[0]);        // front residue
  TEST_ASSERT_EQUAL_INT('c', p[2 * PAGE]); // tail residue
  munmap(p, 3 * PAGE);
}

/* TC3: tail truncation — head survives. */
void test_munmap_tail_truncate(void) {
  char *p = mmap(NULL, 3 * PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  TEST_ASSERT_NOT_NULL(p);
  p[0] = 'a';
  p[PAGE] = 'b';
  TEST_ASSERT_EQUAL_INT(0, munmap(p + PAGE, 2 * PAGE));
  expect_unmapped(p + PAGE);
  TEST_ASSERT_EQUAL_INT('a', p[0]); // head residue
  munmap(p, PAGE);
}

/* TC4: head truncation — tail survives. */
void test_munmap_head_truncate(void) {
  char *p = mmap(NULL, 3 * PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  TEST_ASSERT_NOT_NULL(p);
  p[0] = 'a';
  p[PAGE] = 'b';
  TEST_ASSERT_EQUAL_INT(0, munmap(p, PAGE));
  expect_unmapped(p);
  TEST_ASSERT_EQUAL_INT('b', p[PAGE]); // tail residue
  munmap(p + PAGE, 2 * PAGE);
}

/* TC5: cross-region unmap drops both. Two anon pages bump-adjacent. */
void test_munmap_cross_region(void) {
  char *a = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  TEST_ASSERT_NOT_NULL(a);
  char *b = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  TEST_ASSERT_NOT_NULL(b);
  // If the allocator happened to place them adjacently, the unmap still spans
  // both regardless of whether they are one or two regions.
  uint64_t lo = (uint64_t)(a < b ? a : b);
  uint64_t hi = (uint64_t)(a > b ? a : b) + PAGE;
  TEST_ASSERT_EQUAL_INT(0, munmap((void *)lo, (size_t)(hi - lo)));
  expect_unmapped(a);
  expect_unmapped(b);
}

/* TC6: empty interval (nothing mapped there) is a silent no-op success. */
void test_munmap_empty_interval(void) {
  // 0x3f000000 is in free user space (above mmap_brk, below stack/trampoline).
  TEST_ASSERT_EQUAL_INT(0, munmap((void *)0x3f000000UL, PAGE));
}

/* TC7: unaligned addr is rejected with -EINVAL (Linux requires page alignment).
 */
void test_munmap_unaligned_addr(void) {
  char *p = mmap(NULL, 3 * PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQUAL_INT(-1, munmap(p + 2048, PAGE));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
  munmap(p, 3 * PAGE);
}

/* TC8: SHM hole-punch keeps the object + data; remap sees old contents. */
void test_munmap_shm_hole(void) {
  int fd = -1;
  fd = memfd_create("s13_shm_hole", 0);
  if (fd < 0) {
    printf("memfd_create unavailable (fd=%d), skipping SHM hole test\n", fd);
    TEST_IGNORE();
    return;
  }
  TEST_ASSERT_EQUAL_INT(0, ftruncate(fd, 2 * PAGE));

  char *p = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  TEST_ASSERT_NOT_NULL(p);
  p[0] = 42;
  p[PAGE] = 43;

  // Hole-punch the first page; the SHM object survives (refcount balance).
  TEST_ASSERT_EQUAL_INT(0, munmap(p, PAGE));
  expect_unmapped(p);
  TEST_ASSERT_EQUAL_INT(43, p[PAGE]); // tail residue still mapped

  // Remap the punched page from the same object — original data preserved.
  char *q =
      mmap(p, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
  TEST_ASSERT_NOT_NULL(q);
  TEST_ASSERT_EQUAL_INT(42, q[0]); // data survived the hole-punch

  munmap(p, 2 * PAGE);
  close(fd);
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();
#ifndef SANITIZER
  RUN_TEST(test_munmap_full);
  RUN_TEST(test_munmap_hole_middle);
  RUN_TEST(test_munmap_tail_truncate);
  RUN_TEST(test_munmap_head_truncate);
  RUN_TEST(test_munmap_cross_region);
  RUN_TEST(test_munmap_shm_hole);
#endif
  RUN_TEST(test_munmap_empty_interval);
  RUN_TEST(test_munmap_unaligned_addr);
  return UNITY_END();
}

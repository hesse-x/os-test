/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// S13: mprotect interval semantics + PROT flag relaxation.
//
// Before S13 sys_mprotect only changed prot on a region whose start exactly
// matched addr (a sub-interval change rewrote the PTEs but left region->prot
// stale, breaking fork COW), and rejected PROT_GROWSDOWN/PROT_SEM. This file
// exercises:
//   - sub-interval protect (residues keep old prot)
//   - PROT_NONE sub-interval
//   - PROT_GROWSDOWN / PROT_SEM accepted as no-op
//   - fork inherits the split prot (mid vs residue)
//   - unmapped page in the interval → -ENOMEM
//   - cross-region protect

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
#ifndef PROT_GROWSDOWN
#define PROT_GROWSDOWN 0x01000000
#endif
#ifndef PROT_SEM
#define PROT_SEM 0x10
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
  _exit(info->si_code); // SEGV_ACCERR(2) for a protection violation
}

static void install_segv_capture(void) {
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_sigaction = segv_handler;
  act.sa_flags = SA_SIGINFO;
  sigemptyset(&act.sa_mask);
  sigaction(SIGSEGV, &act, NULL);
}

static int reap_child_code(pid_t pid) {
  int status;
  waitpid(pid, &status, 0);
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  return 0;
}

// In a child, a write to fault_page must fault with SEGV_ACCERR.
static void expect_write_denied(void *fault_page) {
  g_fault_page = fault_page;
  pid_t pid = fork();
  if (pid == 0) {
    install_segv_capture();
    *(volatile char *)fault_page = 'x';
    _exit(0);
  }
  TEST_ASSERT_EQUAL_INT(SEGV_ACCERR, reap_child_code(pid));
}

/* TC1: sub-interval protect — middle page goes RO, residues stay RW. */
void test_mprotect_subinterval(void) {
  char *p = mmap(NULL, 3 * PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  TEST_ASSERT_NOT_NULL(p);
  p[0] = 'a';
  p[PAGE] = 'b';
  p[2 * PAGE] = 'c';
  TEST_ASSERT_EQUAL_INT(0, mprotect(p + PAGE, PAGE, PROT_READ));
  expect_write_denied(p + PAGE);
  p[0] = 'A';        // front residue still RW
  p[2 * PAGE] = 'C'; // tail residue still RW
  TEST_ASSERT_EQUAL_INT('A', p[0]);
  TEST_ASSERT_EQUAL_INT('C', p[2 * PAGE]);
  munmap(p, 3 * PAGE);
}

/* TC2: PROT_NONE sub-interval — access faults, neighbor survives. */
void test_mprotect_prot_none_subinterval(void) {
  char *p = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  TEST_ASSERT_NOT_NULL(p);
  p[0] = 'a';
  p[PAGE] = 'b';
  TEST_ASSERT_EQUAL_INT(0, mprotect(p, PAGE, PROT_NONE));
  expect_write_denied(p);
  p[PAGE] = 'B'; // neighbor still RW
  TEST_ASSERT_EQUAL_INT('B', p[PAGE]);
  munmap(p, 2 * PAGE);
}

/* TC3: PROT_GROWSDOWN masked to no-op (no longer -EINVAL). */
void test_mprotect_growsdown_noop(void) {
  char *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  TEST_ASSERT_NOT_NULL(p);
  p[0] = 'a';
  TEST_ASSERT_EQUAL_INT(0, mprotect(p, PAGE, PROT_READ | PROT_GROWSDOWN));
  expect_write_denied(p); // prot collapsed to READ
  mprotect(p, PAGE, PROT_READ | PROT_WRITE);
  munmap(p, PAGE);
}

/* TC4: PROT_SEM masked to no-op. */
void test_mprotect_sem_noop(void) {
  char *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  TEST_ASSERT_NOT_NULL(p);
  p[0] = 'a';
  TEST_ASSERT_EQUAL_INT(0, mprotect(p, PAGE, PROT_READ | PROT_SEM));
  expect_write_denied(p); // prot collapsed to READ
  mprotect(p, PAGE, PROT_READ | PROT_WRITE);
  munmap(p, PAGE);
}

/* TC5: fork inherits the split prot — child's mid page is RO, residue RW. */
void test_mprotect_fork_inherits_split(void) {
  char *p = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  TEST_ASSERT_NOT_NULL(p);
  p[0] = 'a';
  p[PAGE] = 'b';
  TEST_ASSERT_EQUAL_INT(0, mprotect(p, PAGE, PROT_READ)); // first page RO

  pid_t pid = fork();
  if (pid == 0) {
    // Child: write to the RO page must fault; write to the RW page must work.
    g_fault_page = p;
    install_segv_capture();
    *(volatile char *)p = 'x'; // should fault (inherited RO)
    _exit(0);                  // unreachable if the fault delivered
  }
  TEST_ASSERT_EQUAL_INT(SEGV_ACCERR, reap_child_code(pid));

  // The RW residue is writable in the child too — verify in another child.
  pid_t pid2 = fork();
  if (pid2 == 0) {
    p[PAGE] = 'B'; // inherited RW
    _exit(p[PAGE] == 'B' ? 0 : 1);
  }
  int status;
  waitpid(pid2, &status, 0);
  TEST_ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);

  mprotect(p, PAGE, PROT_READ | PROT_WRITE);
  munmap(p, 2 * PAGE);
}

/* TC6: unmapped page in the interval → -ENOMEM (Linux strict). */
void test_mprotect_unmapped_enomem(void) {
  // 0x3f000000 is free user space.
  TEST_ASSERT_EQUAL_INT(-1, mprotect((void *)0x3f000000UL, PAGE, PROT_READ));
  TEST_ASSERT_EQUAL_INT(ENOMEM, errno);
}

/* TC7: cross-region protect — two adjacent anon pages both go RO. */
void test_mprotect_cross_region(void) {
  char *a = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  TEST_ASSERT_NOT_NULL(a);
  char *b = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  TEST_ASSERT_NOT_NULL(b);
  a[0] = 'a';
  b[0] = 'b';
  uint64_t lo = (uint64_t)(a < b ? a : b);
  uint64_t hi = (uint64_t)(a > b ? a : b) + PAGE;
  TEST_ASSERT_EQUAL_INT(0, mprotect((void *)lo, (size_t)(hi - lo), PROT_READ));
  expect_write_denied(a);
  expect_write_denied(b);
  munmap(a, PAGE);
  munmap(b, PAGE);
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();
#ifndef SANITIZER
  RUN_TEST(test_mprotect_subinterval);
  RUN_TEST(test_mprotect_prot_none_subinterval);
  RUN_TEST(test_mprotect_growsdown_noop);
  RUN_TEST(test_mprotect_sem_noop);
  RUN_TEST(test_mprotect_fork_inherits_split);
  RUN_TEST(test_mprotect_cross_region);
#endif
  RUN_TEST(test_mprotect_unmapped_enomem);
  return UNITY_END();
}

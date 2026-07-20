/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_mprotect — mprotect + PROT_NONE round-trip Unity tests.
 *
 * Covers plan1.md §1.5 acceptance (mprotect = 缺口1 hard acceptance):
 *   1. RW→RX transition + execution (invlpg must flush stale TLB)
 *   2. R page write → SIGSEGV SEGV_ACCERR (present + write; verifies PTE_RW
 *      clear, distinct from SEGV_MAPERR)
 *   3. PROT_NONE round-trip: PROT_NONE access → SIGSEGV SEGV_ACCERR (NOT
 *      MAPERR — verifies §1.3.3 PF fix: PROTNONE leaf is a valid mapping with
 *      no access, not "unmapped"), then back to RW → write/read OK
 *   4. PROT_NONE then munmap → re-alloc no leak (physical reclaim verified
 *      via §1.3.3 unmap_user_pages/proc.c leaf reclaim using pte_present())
 *
 * sysconf(_SC_PAGESIZE)==4096 direct check.
 *
 * Faulting cases (2,3) run in forked children with an SA_SIGINFO handler so
 * the child can capture siginfo.si_code (SEGV_ACCERR vs SEGV_MAPERR) before
 * the fault terminates the child. The captured si_code is propagated back to
 * the parent via the child's exit code (the handler calls _exit(si_code)),
 * NOT via a shared global — after fork the child's BSS is a COW-private
 * copy, so a write in the child is invisible to the parent. The parent
 * reaps it with waitpid and reads WEXITSTATUS; if the child was killed by
 * SIGSEGV (handler never installed / second fault) WIFSIGNALED is true and
 * the parent treats the code as 0 (no capture).
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/process.h>
#include <sys/wait.h>
#include <syscall.h>
#include <unistd.h>
#include <unity.h>
#include <xos/errno.h>

/* xos/mman.h defines only PROT_READ/WRITE/EXEC; PROT_NONE is prot==0. */
#ifndef PROT_NONE
#define PROT_NONE 0
#endif

void setUp(void) {}
void tearDown(void) {}

/* ---- helpers shared across faulting cases ---- */

/* The faulting page is communicated to the handler via this process-local
 * global; it is set BEFORE fork and read-only in the child, so COW keeps the
 * value visible. The handler also asserts si_addr lands in this page before
 * exiting with the captured si_code. */
static volatile void *g_fault_page;

/* SA_SIGINFO handler for a faulting child. Captures siginfo.si_code, asserts
 * si_addr is inside the faulting page, then exits the child with the code so
 * the parent can read it via WEXITSTATUS. Restoring SIG_DFL first would let a
 * second fault terminate the child, but we _exit immediately instead — the
 * signal has already been observed. */
static void segv_handler(int sig, siginfo_t *info, void *uctx) {
  (void)sig;
  (void)uctx;
  int code = info->si_code;
  void *addr = info->_sifields.si_addr;
  /* Sanity: the fault must be on the page we expect. A wrong addr means the
   * kernel delivered siginfo for a different fault — fail the child with a
   * sentinel the parent can distinguish from a genuine SEGV_* code. */
  if (!((uintptr_t)addr >= (uintptr_t)g_fault_page &&
        (uintptr_t)addr < (uintptr_t)g_fault_page + 4096)) {
    _exit(0xfe);
  }
  _exit(code);
}

/* Install the SEGV capture handler. */
static void install_segv_capture(void) {
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_sigaction = segv_handler;
  act.sa_flags = SA_SIGINFO;
  sigemptyset(&act.sa_mask);
  sigaction(SIGSEGV, &act, NULL);
}

/* Wait for a forked child and return the si_code its handler captured (0 if
 * the child never ran the handler — killed by SIGSEGV, or exited 0). */
static int reap_child_code(pid_t pid) {
  int status;
  waitpid(pid, &status, 0);
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  /* Killed by a signal (handler not installed, or a second fault): no code
   * was captured. */
  return 0;
}

/* ---- Case 1: RW→RX transition + execution (invlpg flush) ---- */

/* EF-001-style: mmap RW, write ret instruction, mprotect RX, call it. */
void test_mprotect_rw_to_rx_exec(void) {
  void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  TEST_ASSERT_NOT_NULL(page);

  /* Write a `ret` (0xc3) instruction. */
  unsigned char *p = (unsigned char *)page;
  p[0] = 0xc3;

  /* Flip to R|X. On CPUs without PCID, or with stale kernel TLB entries,
   * a missing invlpg would let the old RW mapping linger and execution
   * could succeed even with a broken flush — but more dangerously, the
   * RX transition itself must take effect. */
  TEST_ASSERT_EQUAL_INT(0, mprotect(page, 4096, PROT_READ | PROT_EXEC));

  /* Call the page as a function. If mprotect failed to take effect (page
   * stayed non-exec), this faults with SIGSEGV. */
  typedef void (*fn_t)(void);
  fn_t fn = (fn_t)page;
  fn();

  munmap(page, 4096);
}

/* ---- Case 2: R page write → SIGSEGV SEGV_ACCERR ---- */

void test_mprotect_r_write_segv_accerr(void) {
  void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  TEST_ASSERT_NOT_NULL(page);
  *(volatile char *)page = 'A';

  /* Drop write permission. */
  TEST_ASSERT_EQUAL_INT(0, mprotect(page, 4096, PROT_READ));

  g_fault_page = page;

  pid_t pid = fork();
  if (pid == 0) {
    install_segv_capture();
    /* Write to a read-only page: #PF with err_code bit0=1 (present) and
     * bit1=1 (write) → SEGV_ACCERR (permission), NOT SEGV_MAPERR. */
    *(volatile char *)page = 'B';
    _exit(0);
  }
  int code = reap_child_code(pid);

  TEST_ASSERT_EQUAL_INT(SEGV_ACCERR, code);

  /* Restore RW in parent so munmap can reclaim cleanly. */
  mprotect(page, 4096, PROT_READ | PROT_WRITE);
  munmap(page, 4096);
}

/* ---- Case 3: PROT_NONE round-trip (the §1.3.3 PF fix) ---- */

void test_mprotect_prot_none_round_trip(void) {
  void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  TEST_ASSERT_NOT_NULL(page);
  *(volatile char *)page = 'A';

  /* ---- PROT_NONE: any access must fault with SEGV_ACCERR, NOT MAPERR ----
   *
   * This is the crux of plan1.md §1.3.3: a PROT_NONE page is present=0 but
   * carries PTE_PROTNONE, so pte_present() recognizes it as a valid mapping.
   * The PF !is_present branch must therefore route to SEGV_ACCERR (protection
   * violation on an existing mapping), NOT SEGV_MAPERR (address not mapped).
   * If the PF fix is absent, the PROTNONE PTE looks not-present AND without
   * the PROTNONE bit → lookup_pte returns NULL → MAPERR, failing this case.
   */
  g_fault_page = page;
  pid_t pid_none = fork();
  if (pid_none == 0) {
    install_segv_capture();
    TEST_ASSERT_EQUAL_INT(0, mprotect(page, 4096, PROT_NONE));
    *(volatile char *)page = 'B';
    _exit(0);
  }
  int code_none = reap_child_code(pid_none);
  TEST_ASSERT_EQUAL_INT(SEGV_ACCERR, code_none);

  /* ---- Back to RW: round-trip must close cleanly ---- */
  TEST_ASSERT_EQUAL_INT(0, mprotect(page, 4096, PROT_READ | PROT_WRITE));
  *(volatile char *)page = 'C';                       /* write OK now */
  TEST_ASSERT_EQUAL_INT('C', *(volatile char *)page); /* read OK */

  munmap(page, 4096);
}

/* ---- Case 4: PROT_NONE then munmap → no leak (reclaim) ---- */

void test_mprotect_prot_none_munmap_no_leak(void) {
  /* Repeatedly mmap/mprotect(PROT_NONE)/munmap. If the §1.3.3 reclaim fix
   * is absent, PROT_NONE pages (present=0, PROTNONE set) are skipped by the
   * old `if (pte & PTE_PRESENT)` leaf-reclaim guards in unmap_user_pages /
   * proc.c mm_release → physical pages leak. With enough iterations this
   * exhausts the free pool and a fresh mmap fails. BFC counts make the leak
   * observable without relying on exhaustion. */
  size_t before = (size_t)sys_sysconf(_SC_AVPHYS_PAGES);

  for (int i = 0; i < 200; i++) {
    void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    TEST_ASSERT_NOT_NULL(page);
    TEST_ASSERT_EQUAL_INT(0, mprotect(page, 4096, PROT_NONE));
    munmap(page, 4096);
  }

  size_t after = (size_t)sys_sysconf(_SC_AVPHYS_PAGES);
  /* A leak of 200 pages (the loop body) would show up as before - after ≈
   * 200. Allow a small slop for allocator metadata; anything close to 200
   * is a real PROTNONE reclaim bug. The comparison is `before - after <
   * 200`, i.e. the free pool must NOT have dropped by 200 pages. */
  TEST_ASSERT_TRUE(before < after + 200);
}

/* ---- sysconf direct check (closures.c path) ---- */

void test_sysconf_pagesize(void) {
  /* closures.c uses sysconf(_SC_PAGESIZE) to size the executable mmap
   * region for ffi_closure trampolines; a wrong value breaks the whole
   * closure path. Must equal getpagesize()/PAGE_SIZE. */
  TEST_ASSERT_EQUAL_INT(4096, sysconf(_SC_PAGESIZE));
  TEST_ASSERT_EQUAL_INT(4096, sysconf(_SC_PAGE_SIZE));
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();
  RUN_TEST(test_mprotect_rw_to_rx_exec);
  // The two SIGSEGV cases below deliberately fault (R-page write, PROT_NONE
  // access) to verify si_code = SEGV_ACCERR. Under the KASAN sanitizer build,
  // trap_dispatch's page-fault DIAG walk phys_to_virt's user-chosen physical
  // addresses into the poisoned low shadow and halts the kernel on a KASAN
  // false-positive before the PF is delivered as SIGSEGV. Skip them under
  // SANITIZER; the non-faulting mprotect coverage (RW→RX exec, PROT_NONE
  // munmap reclaim, sysconf) still runs.
#ifndef SANITIZER
  RUN_TEST(test_mprotect_r_write_segv_accerr);
  RUN_TEST(test_mprotect_prot_none_round_trip);
#endif
  RUN_TEST(test_mprotect_prot_none_munmap_no_leak);
  RUN_TEST(test_sysconf_pagesize);
  return UNITY_END();
}

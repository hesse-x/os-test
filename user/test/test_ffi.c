/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_ffi — libffi Unity tests (ffi_worklist §4.C, 4 cases).
 *
 * 1. ffi_call int (add→7) — verifies CIF prep + SysV calling convention +
 *    unix64.S.
 * 2. ffi_call double (mul→12.0) — verifies SSE xmm argument passing (ffi64.c).
 * 3. ffi_closure callback (ffi_prep_closure_loc builds a closure wrapping a
 *    C function, function-pointer call asserts ==7) — verifies the
 * FFI_MMAP_EXEC_WRIT double-map closure path: dlmmap_locked
 * (closures.c:845/867) maps the memfd backing file twice — a R|W writable page
 * (closure data) and a R|X executable page (*code) — and ffi_prep_closure_loc
 * memcpy's the dynamic jump pad into the R|X page (ffi64.c:1201). Per
 * ffi_worklist §1.1 the writable↔executable bridge is file-backed double mmap,
 * not a single RWX page nor mprotect.
 * 4. mprotect independent case — mmap RW, write a `ret` (0xc3), mprotect to
 *    R|X, call the page as a function. Directly verifies sys_mprotect's PTE
 *    re-protection + per-page invlpg (ffi_worklist 缺口1 hard acceptance;
 *    invlpg is a correctness requirement per resolve_cow_fault analogy, not
 *    optional).
 *
 * Cases 1-3 link libffi.so (DT_NEEDED); case 4 needs no libffi symbols but
 * lives here per worklist §4.C grouping.
 */

#include <ffi.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

/* ---- helpers wrapped by the closure in case 3 ---- */

static int ffi_add(int a, int b) { return a + b; }
static double ffi_mul(double a, double b) { return a * b; }

/* closure target for case 3: sums two int args */
static void closure_add(ffi_cif *cif, void *ret, void **args, void *user_data) {
  (void)cif;
  (void)user_data;
  *(int *)ret = *(int *)args[0] + *(int *)args[1];
}

/* ---- Case 1: ffi_call int (add→7) ---- */

void test_ffi_call_int(void) {
  ffi_cif cif;
  ffi_type *arg_types[] = {&ffi_type_sint, &ffi_type_sint};
  TEST_ASSERT_EQUAL_INT(FFI_OK, ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 2,
                                             &ffi_type_sint, arg_types));
  int arg1 = 3, arg2 = 4, result = 0;
  void *args[] = {&arg1, &arg2};
  ffi_call(&cif, FFI_FN(ffi_add), &result, args);
  TEST_ASSERT_EQUAL_INT(7, result);
}

/* ---- Case 2: ffi_call double (mul→12.0, SSE xmm) ---- */

void test_ffi_call_double(void) {
  ffi_cif cif;
  ffi_type *arg_types[] = {&ffi_type_double, &ffi_type_double};
  TEST_ASSERT_EQUAL_INT(FFI_OK, ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 2,
                                             &ffi_type_double, arg_types));
  double arg1 = 3.0, arg2 = 4.0, result = 0.0;
  void *args[] = {&arg1, &arg2};
  ffi_call(&cif, FFI_FN(ffi_mul), &result, args);
  TEST_ASSERT_EQUAL_DOUBLE(12.0, result);
}

/* ---- Case 3: ffi_closure callback (RWX mmap + dynamic trampoline) ---- */

void test_ffi_closure_callback(void) {
  ffi_cif cif;
  ffi_type *arg_types[] = {&ffi_type_sint, &ffi_type_sint};
  TEST_ASSERT_EQUAL_INT(FFI_OK, ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 2,
                                             &ffi_type_sint, arg_types));

  /* ffi_closure_alloc writes the executable address to *code (closures.c:993
   * returns NULL when code==NULL — it is a required out-param, not optional).
   * Under FFI_MMAP_EXEC_WRIT, dlmmap_locked (closures.c:845/867) double-maps
   * the memfd backing file to two distinct VA ranges: a R|W writable page
   * (the returned closure ptr) and a R|X executable page (*code). The two
   * differ by the segment exec offset (mmap_exec_offset, closures.c:881), so
   * the function pointer MUST be the executable *code, not the writable
   * closure — calling closure would jump into a non-executable page. */
  void *code;
  void *closure = ffi_closure_alloc(sizeof(ffi_closure), &code);
  TEST_ASSERT_NOT_NULL(closure);

  /* Bind: codeloc is the executable address (code), per upstream pattern
   * (testsuite/.../cls_sshort.c:36). ffi_prep_closure_loc memcpy's the
   * dynamic jump pad into the R|X page (ffi64.c:1201). */
  int bound = ffi_prep_closure_loc(closure, &cif, closure_add, NULL, code);
  TEST_ASSERT_EQUAL_INT(FFI_OK, bound);

  /* Call through the executable address. A non-executable trampoline page or
   * a wrong jump pad faults here. */
  typedef int (*closure_fn_t)(int, int);
  closure_fn_t fn = (closure_fn_t)code;
  TEST_ASSERT_EQUAL_INT(7, fn(3, 4));

  ffi_closure_free(closure);
}

/* ---- Case 4: mprotect RW→RX + execution (invlpg flush) ---- */

void test_ffi_mprotect_rw_to_rx_exec(void) {
  void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  TEST_ASSERT_NOT_NULL(page);

  /* Write a `ret` (0xc3) instruction. */
  unsigned char *p = (unsigned char *)page;
  p[0] = 0xc3;

  /* Flip to R|X. sys_mprotect must re-program PTE PTE_NX clear + invlpg per
   * page (resolve_cow_fault analogy); a missing invlpg lets stale TLB keep
   * the old RW mapping. */
  TEST_ASSERT_EQUAL_INT(0, mprotect(page, 4096, PROT_READ | PROT_EXEC));

  /* Call the page as a function. If mprotect failed to take effect (page
   * stayed non-exec), this faults with SIGSEGV. */
  typedef void (*fn_t)(void);
  fn_t fn = (fn_t)page;
  fn();

  munmap(page, 4096);
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();
  RUN_TEST(test_ffi_call_int);
  RUN_TEST(test_ffi_call_double);
  RUN_TEST(test_ffi_closure_callback);
  RUN_TEST(test_ffi_mprotect_rw_to_rx_exec);
  return UNITY_END();
}

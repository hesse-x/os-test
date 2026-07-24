/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* S19 §2 — CLONE_PARENT_SETTID / CLONE_CHILD_SETTID with a bad pointer must
 * return -EFAULT instead of kernel-panic'ing on a #PF. The kernel writes the
 * tid via copy_to_user (access_ok + extable fixup), and on fault rolls back
 * everything allocated so far (mm/proc/files/fpu/stack) and returns -EFAULT.
 *
 * Cases:
 *   - PARENT_SETTID with an unmapped user pointer → -EFAULT
 *   - CHILD_SETTID with an unmapped user pointer → -EFAULT
 *   - CHILD_SETTID with a kernel-VMA pointer (>= KERNEL_VMA_BOUNDARY) →
 *     access_ok rejects it → -EFAULT
 *   - good pointers: tid written to both, == returned pid; child reaped.
 *
 * The trampoline is the same raw-clone asm as test_clone_exit_signal.c. On the
 * fault path the syscall returns a negative errno, so the asm takes the parent
 * branch (rax != 0) and never runs the child body — no child exists to reap. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <syscall.h> // SYS_CLONE, __syscall5
#include <unistd.h>
#include <unity.h>
#include <xos/errno.h>

void setUp(void) {}
void tearDown(void) {}

#define C_VM 0x00000100
#define C_FILES 0x00000400
#define C_PARENT_SETTID 0x00100000
#define C_CHILD_SETTID 0x01000000

/* A user address that is unmapped in this process: high-ish canonical user
 * address, never touched by the libc layout. access_ok passes (it is below the
 * kernel boundary) but the copy_to_user load/store faults → fixup → -EFAULT. */
#define BAD_USER_PTR ((void *)0x500000000ULL)
/* A kernel-VMA address: access_ok rejects it outright (>= kernel boundary). */
#define KERNEL_PTR ((void *)0xFFFFFFFF80100000ULL)

/* raw_clone(flags, stack_top, ptid, ctid): child runs fn then _exit(0). */
static int64_t raw_clone(uint64_t flags, void *stack_top, void *ptid,
                         void *ctid, void (*fn)(void)) {
  *(void (**)(void))((uintptr_t)stack_top - sizeof(void *)) = fn;
  uintptr_t child_sp = (uintptr_t)stack_top - sizeof(void *);

  int64_t r;
  /* arg3 (parent_tid) in rdx, arg4 (child_tid) in r10, arg5 (tls) in r8. */
  register uint64_t r10 __asm__("r10") = (uint64_t)(uintptr_t)ctid;
  register uint64_t r8 __asm__("r8") = 0; /* tls */
  __asm__ volatile("syscall\n"
                   "testq %%rax, %%rax\n"
                   "jnz 1f\n"
                   "movq %%rsi, %%rsp\n"
                   "xorq %%rbp, %%rbp\n"
                   "popq %%rdi\n"
                   "callq *%%rdi\n"
                   "movq $0, %%rdi\n"
                   "callq _exit\n"
                   "1:\n"
                   : "=a"(r)
                   : "a"((int64_t)SYS_CLONE), "D"((int64_t)flags),
                     "S"((int64_t)child_sp), "d"((int64_t)(uintptr_t)ptid),
                     "r"(r10), "r"(r8)
                   : "rcx", "r11", "memory");
  return r;
}

static void *alloc_stack(void) {
  void *stk = mmap(NULL, 64 * 1024, PROT_READ | PROT_WRITE,
                   MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  TEST_ASSERT_NOT_EQUAL(MAP_FAILED, stk);
  return (void *)((uintptr_t)stk + 64 * 1024);
}

static void child_just_exit(void) {}

/* ---- 1. PARENT_SETTID with an unmapped user pointer → -EFAULT ---- */
void test_clone_settid_parent_fault(void) {
  void *stk = alloc_stack();
  int64_t r = raw_clone(C_VM | C_FILES | C_PARENT_SETTID, stk, BAD_USER_PTR,
                        NULL, child_just_exit);
  TEST_ASSERT_EQUAL_INT(-EFAULT, r);
}

/* ---- 2. CHILD_SETTID with an unmapped user pointer → -EFAULT ---- */
void test_clone_settid_child_fault(void) {
  void *stk = alloc_stack();
  int64_t r = raw_clone(C_VM | C_FILES | C_CHILD_SETTID, stk, NULL,
                        BAD_USER_PTR, child_just_exit);
  TEST_ASSERT_EQUAL_INT(-EFAULT, r);
}

/* ---- 3. CHILD_SETTID with a kernel-VMA pointer → -EFAULT (access_ok) ---- */
void test_clone_settid_child_kernel_ptr(void) {
  void *stk = alloc_stack();
  int64_t r = raw_clone(C_VM | C_FILES | C_CHILD_SETTID, stk, NULL, KERNEL_PTR,
                        child_just_exit);
  TEST_ASSERT_EQUAL_INT(-EFAULT, r);
}

/* ---- 4. good pointers: tid written to both, == pid; child reaped ---- */
void test_clone_settid_good_pointers(void) {
  pid_t ptid = 0, ctid = 0;
  void *stk = alloc_stack();
  int64_t r = raw_clone(C_VM | C_FILES | C_PARENT_SETTID | C_CHILD_SETTID, stk,
                        &ptid, &ctid, child_just_exit);
  TEST_ASSERT_TRUE(r > 0);
  TEST_ASSERT_EQUAL_INT((int)r, ptid);
  TEST_ASSERT_EQUAL_INT((int)r, ctid);

  int status = 0;
  TEST_ASSERT_EQUAL_INT((int)r, waitpid((pid_t)r, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_clone_settid_parent_fault);
  RUN_TEST(test_clone_settid_child_fault);
  RUN_TEST(test_clone_settid_child_kernel_ptr);
  RUN_TEST(test_clone_settid_good_pointers);
  return UNITY_END();
}

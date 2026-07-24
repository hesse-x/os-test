/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* S19 §1 — clone exit_signal extraction. Verification test for the kernel
 * change (already implemented in the working tree):
 *   - clone(flags|SIGUSR1, ...) → parent is notified with SIGUSR1 on child
 *     exit, NOT the hardcoded SIGCHLD. exit_signal is the low byte of flags.
 *   - CLONE_THREAD forces exit_signal=0 → a thread exit does NOT notify the
 *     parent (threads report via clear_tid_addr + futex, not a signal).
 *   - exit_signal >= NSIG is rejected with -EINVAL before any child is made.
 *
 * We cannot call clone through a C wrapper: after the clone child returns from
 * the syscall it is still in the parent's stack frame (rbp points at the
 * parent stack), and the wrapper's epilogue (leave; ret) switches rsp back to
 * the parent stack. So the syscall is issued with inline asm that, on the
 * child path (rax==0), resets rsp to the child stack top, pops the fn pointer
 * stored there, calls it, then _exit(0). The parent returns in rax with the
 * child tid (or a negative errno). This mirrors __libc_clone_thread in
 * pthread.cc but stores the fn pointer on the child stack instead of in a TCB
 * (no pthread dependency for these raw-clone tests). */

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/process.h> // fork, setuid
#include <sys/wait.h>
#include <syscall.h> // SYS_CLONE
#include <unistd.h>
#include <unity.h>
#include <xos/errno.h>
#include <xos/signal.h> // NSIG

void setUp(void) {}
void tearDown(void) {}

/* clone flag bits (mirror the kernel's uapi values) */
#define C_VM 0x00000100
#define C_FILES 0x00000400
#define C_SIGHAND 0x00000800
#define C_THREAD 0x00010000

/* clone_run(flags, fn): issue raw clone; the child resets rsp to `stack_top`,
 * pops the fn pointer pushed there, calls fn(), then _exit(0). Returns the
 * child tid (>0) or a negative errno in the parent. */
static int64_t clone_run(uint64_t flags, void (*fn)(void), void *stack_top) {
  /* Stash fn at the child's new stack top so the trampoline can pop it. */
  *(void (**)(void))((uintptr_t)stack_top - sizeof(void *)) = fn;
  uintptr_t child_sp = (uintptr_t)stack_top - sizeof(void *);

  int64_t r;
  /* syscall calling convention: arg4 in r10, arg5 in r8 (not rdi/rsi/rdx/rcx).
   * Bind them to the right physical registers so child_tid/tls are actually
   * passed to the kernel (a generic "r" constraint could land in any GPR). */
  register uint64_t r10 __asm__("r10") = 0; /* child_tid */
  register uint64_t r8 __asm__("r8") = 0;   /* tls */
  __asm__ volatile("syscall\n"
                   "testq %%rax, %%rax\n"
                   "jnz 1f\n" /* parent: rax != 0 */
                   /* child: rax == 0 — run on our own stack */
                   "movq %%rsi, %%rsp\n" /* rsp = child_sp */
                   "xorq %%rbp, %%rbp\n" /* no frame */
                   "popq %%rdi\n"   /* rdi = fn (popped from child stack) */
                   "callq *%%rdi\n" /* fn() */
                   "movq $0, %%rdi\n"
                   "callq _exit\n"
                   "1:\n"
                   : "=a"(r)
                   : "a"((int64_t)SYS_CLONE), "D"((int64_t)flags),
                     "S"((int64_t)child_sp), "d"((int64_t)0) /* parent_tid */,
                     "r"(r10), "r"(r8)
                   : "rcx", "r11", "memory");
  return r;
}

static void *alloc_stack(void) {
  void *stk = mmap(NULL, 64 * 1024, PROT_READ | PROT_WRITE,
                   MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  TEST_ASSERT_NOT_NULL(stk);
  TEST_ASSERT_NOT_EQUAL(MAP_FAILED, stk);
  return (void *)((uintptr_t)stk + 64 * 1024); /* top of the region */
}

static volatile int g_last_sig;
static void sig_handler(int sig) { g_last_sig = sig; }

static void child_just_exit(void) { /* returns → trampoline _exit(0) */ }

/* ---- 1. clone(SIGUSR1) notifies parent with SIGUSR1, not SIGCHLD ---- */
void test_clone_usr1_notifies_parent(void) {
  g_last_sig = 0;
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = sig_handler;
  sigaction(SIGUSR1, &act, NULL);
  sigaction(SIGCHLD, &act, NULL);

  void *stk = alloc_stack();
  int64_t r = clone_run(C_VM | C_FILES | SIGUSR1, child_just_exit, stk);
  TEST_ASSERT_TRUE(r > 0);

  int status = 0;
  TEST_ASSERT_EQUAL_INT((int)r, waitpid((pid_t)r, &status, 0));

  /* Let the signal be delivered (SIGCHLD/SIGUSR1 are posted async). The child
   * exit with exit_signal=SIGUSR1 must set g_last_sig to SIGUSR1, proving the
   * low byte of flags was honored rather than the old hardcoded SIGCHLD. */
  for (int i = 0; i < 10000 && g_last_sig == 0; i++)
    sched_yield();
  TEST_ASSERT_EQUAL_INT(SIGUSR1, g_last_sig);

  memset(&act, 0, sizeof(act));
  act.sa_handler = SIG_DFL;
  sigaction(SIGUSR1, &act, NULL);
  sigaction(SIGCHLD, &act, NULL);
}

/* ---- 2. CLONE_THREAD forces exit_signal=0 → parent gets NO signal ---- */
void test_clone_thread_no_signal(void) {
  g_last_sig = 0;
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = sig_handler;
  sigaction(SIGUSR1, &act, NULL);
  sigaction(SIGCHLD, &act, NULL);

  void *stk = alloc_stack();
  /* exit_signal low byte is SIGUSR1, but CLONE_THREAD overrides it to 0. The
   * thread exits without notifying the parent; g_last_sig must stay 0. */
  int64_t r = clone_run(C_VM | C_FILES | C_SIGHAND | C_THREAD | SIGUSR1,
                        child_just_exit, stk);
  TEST_ASSERT_TRUE(r > 0);

  /* The thread is reaped by sched_task_reap (no clear_tid_addr, no futex);
   * give it time to run and exit, and confirm no signal arrived. */
  for (int i = 0; i < 5000; i++) {
    sched_yield();
    if (g_last_sig != 0)
      break;
  }
  TEST_ASSERT_EQUAL_INT(0, g_last_sig);

  memset(&act, 0, sizeof(act));
  act.sa_handler = SIG_DFL;
  sigaction(SIGUSR1, &act, NULL);
  sigaction(SIGCHLD, &act, NULL);
}

/* ---- 3. invalid exit_signal (>= NSIG) rejected with -EINVAL ---- */
void test_clone_invalid_exit_signal(void) {
  void *stk = alloc_stack();
  /* 0x41 == 65 == NSIG; the kernel rejects exit_signal != 0 && >= NSIG. No
   * child is created, so nothing to reap. */
  int64_t r = clone_run(C_VM | C_FILES | 0x41, child_just_exit, stk);
  TEST_ASSERT_EQUAL_INT(-EINVAL, r);
}

/* ---- 4. CLONE_THREAD child thread inherits the parent's uid ---- */
/* Pins the §6.3 identity-inheritance fix: a non-root parent's CLONE_THREAD
 * child must read the parent's uid, not the proc_create default of 0. The
 * thread reads its uid via getuid() and encodes the result in the parent's
 * exit status through a shared word. */
static volatile int g_thread_uid_ok;
static void read_uid_thread(void) {
  /* A CLONE_VM thread shares the address space, so g_thread_uid_ok is visible
   * to both. getuid() reads the inherited identity. */
  g_thread_uid_ok = (getuid() == 1000) ? 1 : 0;
}

void test_clone_thread_inherits_uid(void) {
  /* Drop to uid 1000 in a child process so the parent test stays root. */
  pid_t child = fork();
  if (child == 0) {
    if (setuid(1000) != 0)
      _exit(2);
    if (getuid() != 1000)
      _exit(3);

    g_thread_uid_ok = 0;
    void *stk = alloc_stack();
    int64_t r =
        clone_run(C_VM | C_FILES | C_SIGHAND | C_THREAD, read_uid_thread, stk);
    if (r <= 0)
      _exit(4);

    /* Wait for the thread to run and write the shared flag. */
    for (int i = 0; i < 5000 && g_thread_uid_ok == 0; i++)
      sched_yield();
    _exit(g_thread_uid_ok == 1 ? 0 : 5);
  }
  int status = 0;
  TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_clone_usr1_notifies_parent);
  RUN_TEST(test_clone_thread_no_signal);
  RUN_TEST(test_clone_invalid_exit_signal);
  RUN_TEST(test_clone_thread_inherits_uid);
  return UNITY_END();
}

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "user/test/test_helpers.h"
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

/* S04: sigaltstack + SA_ONSTACK. A handler installed with SA_ONSTACK runs on
 * the alternate signal stack, so its rsp must lie within [ss_sp, ss_sp+ss_size)
 * and sigaltstack(NULL,&old) queried from the handler must report SS_ONSTACK.
 * After return the main stack rsp is unchanged. */
static void *altstack_base;
static size_t altstack_size;
static volatile uintptr_t handler_rsp;
static volatile int handler_onstack_flag;
static volatile int handler_fired;

static void onstack_handler(int sig) {
  (void)sig;
  /* Capture the handler's own stack pointer. Naked asm so we read the actual
   * rsp at function entry (the SysV prologue would have pushed rbp first). */
  uintptr_t sp;
  __asm__ volatile("mov %%rsp, %0" : "=r"(sp));
  handler_rsp = sp;
  handler_fired = 1;

  stack_t cur;
  memset(&cur, 0, sizeof(cur));
  sigaltstack(NULL, &cur);
  handler_onstack_flag = (cur.ss_flags & SS_ONSTACK) ? 1 : 0;
}

void test_sigaltstack_onstack_delivery(void) {
  /* Allocate the alternate stack with mmap so it does not consume main-stack
   * guard pages and survives across the handler. */
  altstack_size = SIGSTKSZ;
  void *stack = mmap(NULL, altstack_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  TEST_ASSERT_NOT_NULL(stack);
  altstack_base = stack;

  stack_t ss;
  memset(&ss, 0, sizeof(ss));
  ss.ss_sp = stack;
  ss.ss_size = altstack_size;
  ss.ss_flags = 0;
  TEST_ASSERT_EQUAL_INT(0, sigaltstack(&ss, NULL));

  /* Record the pre-delivery main-thread rsp so we can assert it is restored. */
  uintptr_t main_rsp;
  __asm__ volatile("mov %%rsp, %0" : "=r"(main_rsp));

  handler_fired = 0;
  handler_onstack_flag = -1;
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = onstack_handler;
  act.sa_flags = SA_ONSTACK;
  TEST_ASSERT_EQUAL_INT(0, sigaction(SIGUSR1, &act, NULL));

  raise(SIGUSR1);

  TEST_ASSERT_EQUAL_INT(1, handler_fired);
  /* Handler rsp must be inside the altstack region. */
  TEST_ASSERT_TRUE((uintptr_t)altstack_base <= handler_rsp &&
                   handler_rsp < (uintptr_t)altstack_base + altstack_size);
  TEST_ASSERT_EQUAL_INT(1, handler_onstack_flag);

  /* SS_ONSTACK must be cleared after return. */
  stack_t after;
  memset(&after, 0, sizeof(after));
  sigaltstack(NULL, &after);
  TEST_ASSERT_FALSE(after.ss_flags & SS_ONSTACK);

  /* Main-thread rsp unchanged after return. */
  uintptr_t main_rsp_after;
  __asm__ volatile("mov %%rsp, %0" : "=r"(main_rsp_after));
  TEST_ASSERT_EQUAL_INT(main_rsp, main_rsp_after);

  /* Restore default action and disable the altstack. */
  memset(&act, 0, sizeof(act));
  act.sa_handler = SIG_DFL;
  sigaction(SIGUSR1, &act, NULL);
  memset(&ss, 0, sizeof(ss));
  ss.ss_flags = SS_DISABLE;
  sigaltstack(&ss, NULL);
  munmap(stack, altstack_size);
}

/* S04: SS_DISABLE. A disabled altstack is reported as such, and an SA_ONSTACK
 * signal then runs on the DEFAULT stack (rsp NOT in the altstack region). We
 * verify in a forked child so a misbehaving default-stack check cannot corrupt
 * the parent's test state. */
static volatile uintptr_t disabled_handler_rsp;
static volatile int disabled_handler_fired;
static void *disabled_alt_base;
static size_t disabled_alt_size;
static void disabled_handler(int sig) {
  (void)sig;
  uintptr_t sp;
  __asm__ volatile("mov %%rsp, %0" : "=r"(sp));
  disabled_handler_rsp = sp;
  disabled_handler_fired = 1;
}

void test_sigaltstack_disabled_runs_on_default_stack(void) {
  pid_t child = fork();
  if (child == 0) {
    disabled_alt_size = SIGSTKSZ;
    void *stack = mmap(NULL, disabled_alt_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (!stack)
      _exit(99);
    disabled_alt_base = stack;

    stack_t ss;
    memset(&ss, 0, sizeof(ss));
    ss.ss_sp = stack;
    ss.ss_size = disabled_alt_size;
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);

    /* Now disable it. */
    memset(&ss, 0, sizeof(ss));
    ss.ss_flags = SS_DISABLE;
    sigaltstack(&ss, NULL);

    stack_t cur;
    memset(&cur, 0, sizeof(cur));
    sigaltstack(NULL, &cur);
    if (!(cur.ss_flags & SS_DISABLE))
      _exit(100);

    /* Record a low bound of the main stack so we can confirm the handler ran
     * on the main stack (rsp well below the altstack region we chose, which is
     * a fresh mmap far from the main stack). */
    uintptr_t main_sp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(main_sp));

    disabled_handler_fired = 0;
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = disabled_handler;
    act.sa_flags = SA_ONSTACK;
    sigaction(SIGUSR1, &act, NULL);
    raise(SIGUSR1);

    if (!disabled_handler_fired)
      _exit(101);
    /* Handler ran on the default stack: rsp is in the main-stack region
     * (close to main_sp), NOT inside the mmap'd altstack. The altstack region
     * is a fresh mmap far below the main stack, so "on the default stack" means
     * rsp is NOT within [alt_base, alt_base+alt_size). */
    int on_default = ((uintptr_t)disabled_alt_base > disabled_handler_rsp ||
                      disabled_handler_rsp >=
                          (uintptr_t)disabled_alt_base + disabled_alt_size);
    /* The main stack grows down, so a handler running on it has rsp at or below
     * main_sp, within a few pages of stack use (not 0x4000 above it, which the
     * downward growth makes impossible). Confirm the handler rsp is near the
     * main stack — below main_sp but not buried unrealistically deep — and far
     * from the mmap'd altstack region. */
    int near_main = (disabled_handler_rsp <= main_sp &&
                     disabled_handler_rsp > main_sp - 0x4000 &&
                     (disabled_handler_rsp < (uintptr_t)disabled_alt_base ||
                      disabled_handler_rsp >=
                          (uintptr_t)disabled_alt_base + disabled_alt_size));
    if (!on_default || !near_main)
      _exit(102);
    _exit(0);
  }
  int status = 0;
  pid_t ret = waitpid(child, &status, 0);
  TEST_ASSERT_EQUAL_INT(child, ret);
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
}

/* S04: EINVAL / ENOMEM validation. ss_flags other than SS_DISABLE/SS_AUTODISARM
 * is rejected; an enabled stack smaller than MINSIGSTKSZ is rejected. */
void test_sigaltstack_validation(void) {
  /* Query-only call with both args NULL succeeds. */
  TEST_ASSERT_EQUAL_INT(0, sigaltstack(NULL, NULL));

  /* Bogus flags → EINVAL. */
  stack_t ss;
  memset(&ss, 0, sizeof(ss));
  ss.ss_sp = (void *)0x10000;
  ss.ss_size = SIGSTKSZ;
  ss.ss_flags = 0x40000000; /* an unsupported flag bit */
  TEST_ASSERT_EQUAL_INT(-1, sigaltstack(&ss, NULL));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);

  /* Enabled stack smaller than MINSIGSTKSZ → ENOMEM. */
  memset(&ss, 0, sizeof(ss));
  ss.ss_sp = (void *)0x10000;
  ss.ss_size = MINSIGSTKSZ - 1;
  ss.ss_flags = 0;
  TEST_ASSERT_EQUAL_INT(-1, sigaltstack(&ss, NULL));
  TEST_ASSERT_EQUAL_INT(ENOMEM, errno);

  /* DISABLE ignores size (Linux accepts size 0 with SS_DISABLE). */
  memset(&ss, 0, sizeof(ss));
  ss.ss_flags = SS_DISABLE;
  ss.ss_size = 0;
  TEST_ASSERT_EQUAL_INT(0, sigaltstack(&ss, NULL));
  stack_t cur;
  memset(&cur, 0, sizeof(cur));
  sigaltstack(NULL, &cur);
  TEST_ASSERT_TRUE(cur.ss_flags & SS_DISABLE);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_sigaltstack_onstack_delivery);
  RUN_TEST(test_sigaltstack_disabled_runs_on_default_stack);
  RUN_TEST(test_sigaltstack_validation);
  return UNITY_END();
}

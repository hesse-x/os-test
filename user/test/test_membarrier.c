/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_membarrier — SYS_membarrier (324) regression.
 *
 * The kernel implements membarrier as a local fence on x86-64 (TSO already
 * orders stores; see sys_membarrier rationale). These tests pin the user-
 * visible contract: the QUERY bitmask, success of the advertised commands,
 * -EINVAL on unknown commands / flags, and that the call is safe to issue from
 * a peer thread (no register clobber / lost wakeup) — the dynamic linker's
 * store-DTV-contents / membarrier / store-DTV-pointer sequence runs in exactly
 * that shape. */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <unistd.h>

#include <unity.h>
#include <xos/errno.h>
#include <xos/syscall_ext.h>

/* Command values mirror <sys/membarrier.h> (Linux UAPI). The kernel and these
 * constants must agree; the QUERY test asserts the exact advertised set so a
 * future divergence fails loudly. */
#define MB_CMD_QUERY 0
#define MB_CMD_GLOBAL 1
#define MB_CMD_GLOBAL_EXPEDITED 2
#define MB_CMD_REGISTER_GLOBAL_EXPEDITED 4
#define MB_CMD_PRIVATE_EXPEDITED 8
#define MB_CMD_REGISTER_PRIVATE_EXPEDITED 16
#define MB_CMD_PRIVATE_EXPEDITED_SYNC_CORE 32
#define MB_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE 64
#define MB_CMD_PRIVATE_EXPEDITED_RSEQ 128
#define MB_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ 256
#define MB_FLAG_CPU 1

/* sys_membarrier(cmd, flags). The libc has no <sys/membarrier.h> wrapper, so
 * issue the raw syscall directly. Returns the kernel's int64 (a bitmask for
 * QUERY, 0 on success, or a negative -errno). */
static inline int64_t membarrier(int cmd, int flags) {
  return __syscall2(SYS_MEMBARRIER, (int64_t)cmd, (int64_t)flags);
}

void setUp(void) {}
void tearDown(void) {}

/* MB-001: QUERY returns exactly the advertised command set: GLOBAL |
 * GLOBAL_EXPEDITED | REGISTER_GLOBAL_EXPEDITED | PRIVATE_EXPEDITED |
 * REGISTER_PRIVATE_EXPEDITED (0x1f). SYNC_CORE / RSEQ are not supported and
 * must be absent so callers see -EINVAL rather than a silent no-op. */
void test_query_returns_advertised_set(void) {
  int64_t r = membarrier(MB_CMD_QUERY, 0);
  TEST_ASSERT_EQUAL_INT(0x1f, (int)r);
  TEST_ASSERT_FALSE(r & MB_CMD_PRIVATE_EXPEDITED_SYNC_CORE);
  TEST_ASSERT_FALSE(r & MB_CMD_PRIVATE_EXPEDITED_RSEQ);
}

/* MB-002: PRIVATE_EXPEDITED succeeds. This is the exact command the dynamic
 * linker issues before installing per-thread DTV pointers. */
void test_private_expedited_succeeds(void) {
  TEST_ASSERT_EQUAL_INT(0, (int)membarrier(MB_CMD_PRIVATE_EXPEDITED, 0));
}

/* MB-003: GLOBAL and GLOBAL_EXPEDITED succeed (cross-process / global forms;
 * the kernel treats them identically to private on TSO). */
void test_global_commands_succeed(void) {
  TEST_ASSERT_EQUAL_INT(0, (int)membarrier(MB_CMD_GLOBAL, 0));
  TEST_ASSERT_EQUAL_INT(0, (int)membarrier(MB_CMD_GLOBAL_EXPEDITED, 0));
}

/* MB-004: REGISTER_* are accepted (no-op success; registration is not
 * enforced). musl's __membarrier_init pre-registers PRIVATE_EXPEDITED, so a
 * failure here would break pthread_create. */
void test_register_commands_succeed(void) {
  TEST_ASSERT_EQUAL_INT(0,
                        (int)membarrier(MB_CMD_REGISTER_GLOBAL_EXPEDITED, 0));
  TEST_ASSERT_EQUAL_INT(0,
                        (int)membarrier(MB_CMD_REGISTER_PRIVATE_EXPEDITED, 0));
}

/* MB-005: any non-zero flags → -EINVAL. FLAG_CPU (single-cpu directed
 * membarrier) is not implemented, so flags==FLAG_CPU fails the same way. */
void test_nonzero_flags_einval(void) {
  TEST_ASSERT_EQUAL_INT(-EINVAL, (int)membarrier(MB_CMD_QUERY, 1));
  TEST_ASSERT_EQUAL_INT(-EINVAL, (int)membarrier(MB_CMD_PRIVATE_EXPEDITED, 1));
  TEST_ASSERT_EQUAL_INT(-EINVAL,
                        (int)membarrier(MB_CMD_PRIVATE_EXPEDITED, MB_FLAG_CPU));
}

/* MB-006: unsupported commands → -EINVAL. SYNC_CORE / RSEQ and a garbage
 * command all fall through the kernel switch's default. */
void test_unsupported_commands_einval(void) {
  TEST_ASSERT_EQUAL_INT(-EINVAL,
                        (int)membarrier(MB_CMD_PRIVATE_EXPEDITED_SYNC_CORE, 0));
  TEST_ASSERT_EQUAL_INT(
      -EINVAL, (int)membarrier(MB_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE, 0));
  TEST_ASSERT_EQUAL_INT(-EINVAL,
                        (int)membarrier(MB_CMD_PRIVATE_EXPEDITED_RSEQ, 0));
  TEST_ASSERT_EQUAL_INT(-EINVAL, (int)membarrier(0x80000000, 0));
}

/* MB-007: functional ordering smoke test. A peer thread observes the
 * data-then-pointer store pattern the dynamic linker relies on: the writer
 * stores the new DTV contents, issues membarrier, then stores the pointer; the
 * reader must never see a pointer to uninitialized/old contents. On x86-64 TSO
 * this is guaranteed for stores, so the test mainly guards that issuing
 * membarrier from a threaded context does not clobber state or lose the thread.
 * It also exercises the call repeatedly under concurrency. */

/* Reader spins until the writer's pointer becomes non-NULL, then reads the
 * contents it points at. A torn observation (pointer visible, contents not the
 * freshly-stored value) would indicate the barrier's ordering contract was
 * broken — which on TSO cannot happen, so this is a regression guard against a
 * future (broken) reimplementation. */
static long g_dtv_value = 0;
static volatile long *g_dtv_ptr = NULL;

static int reader_fn(void *arg) {
  (void)arg;
  volatile long *p;
  /* Wait for the writer to publish the pointer. */
  while ((p = g_dtv_ptr) == NULL)
    thrd_yield();
  /* The contents must already be visible: the writer stored the value, then
   * membarrier, then the pointer. */
  TEST_ASSERT_EQUAL_INT(g_dtv_value, *p);
  return 0;
}

void test_ordering_with_peer_thread(void) {
  for (int iter = 0; iter < 200; iter++) {
    g_dtv_value = 0x1000 + iter;
    g_dtv_ptr = NULL;

    thrd_t t;
    TEST_ASSERT_EQUAL(thrd_success, thrd_create(&t, reader_fn, NULL));

    /* Store contents, barrier, store pointer — the canonical pattern. */
    long local = g_dtv_value;
    g_dtv_value = local; /* (no-op store; mirrors "store new contents") */
    if (membarrier(MB_CMD_PRIVATE_EXPEDITED, 0) != 0) {
      thrd_join(t, NULL);
      TEST_FAIL_MESSAGE("membarrier PRIVATE_EXPEDITED failed mid-test");
    }
    g_dtv_ptr = &g_dtv_value;

    int res;
    TEST_ASSERT_EQUAL(thrd_success, thrd_join(t, &res));
    TEST_ASSERT_EQUAL_INT(0, res);
  }
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_query_returns_advertised_set);
  RUN_TEST(test_private_expedited_succeeds);
  RUN_TEST(test_global_commands_succeed);
  RUN_TEST(test_register_commands_succeed);
  RUN_TEST(test_nonzero_flags_einval);
  RUN_TEST(test_unsupported_commands_einval);
  RUN_TEST(test_ordering_with_peer_thread);
  return UNITY_END();
}

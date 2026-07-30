/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * C11 <threads.h> regression. musl src/thread/{thrd,mtx,cnd,tss,call_once}_*.c
 * are compiled into libc by the src/thread glob in pthread.cmake; the C11
 * symbols are exported via libc.map's <threads.h> block (thrd_current /
 * thrd_detach / tss_get are weak_alias of the pthread equivalents). This test
 * exercises the C11 surface end-to-end so a dropped export or a missing source
 * shows up as a link or runtime failure.
 */

#include <errno.h>
#include <stdio.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

/* ---- thrd: create / join / exit retval ---- */
static int thread_basic_fn(void *arg) {
  int *p = (int *)arg;
  *p = 42;
  return 7;
}

void test_thrd_create_join(void) {
  thrd_t t;
  int val = 0;
  TEST_ASSERT_EQUAL_INT(thrd_success, thrd_create(&t, thread_basic_fn, &val));
  int res = -1;
  TEST_ASSERT_EQUAL_INT(thrd_success, thrd_join(t, &res));
  TEST_ASSERT_EQUAL_INT(42, val);
  TEST_ASSERT_EQUAL_INT(7, res);
}

/* ---- mtx: plain lock / unlock, recursive ---- */
static int counter;
static mtx_t cnt_mtx;

static int incrementer_fn(void *arg) {
  int iters = *(int *)arg;
  for (int i = 0; i < iters; i++) {
    mtx_lock(&cnt_mtx);
    counter++;
    mtx_unlock(&cnt_mtx);
  }
  return 0;
}

void test_mtx_lock_stress(void) {
  TEST_ASSERT_EQUAL_INT(thrd_success, mtx_init(&cnt_mtx, mtx_plain));
  counter = 0;
  enum { N = 4, ITERS = 2000 };
  int iters = ITERS;
  thrd_t ts[N];
  for (int i = 0; i < N; i++)
    TEST_ASSERT_EQUAL_INT(thrd_success,
                          thrd_create(&ts[i], incrementer_fn, &iters));
  for (int i = 0; i < N; i++)
    TEST_ASSERT_EQUAL_INT(thrd_success, thrd_join(ts[i], NULL));
  TEST_ASSERT_EQUAL_INT(N * ITERS, counter);
  mtx_destroy(&cnt_mtx);
}

void test_mtx_recursive(void) {
  mtx_t m;
  TEST_ASSERT_EQUAL_INT(thrd_success, mtx_init(&m, mtx_recursive));
  TEST_ASSERT_EQUAL_INT(thrd_success, mtx_lock(&m));
  TEST_ASSERT_EQUAL_INT(thrd_success, mtx_lock(&m)); /* reentrant */
  TEST_ASSERT_EQUAL_INT(thrd_success, mtx_unlock(&m));
  TEST_ASSERT_EQUAL_INT(thrd_success, mtx_unlock(&m));
  mtx_destroy(&m);
}

void test_mtx_trylock(void) {
  mtx_t m;
  TEST_ASSERT_EQUAL_INT(thrd_success, mtx_init(&m, mtx_plain));
  TEST_ASSERT_EQUAL_INT(thrd_success, mtx_trylock(&m));
  TEST_ASSERT_EQUAL_INT(thrd_busy, mtx_trylock(&m)); /* already held */
  mtx_unlock(&m);
  mtx_destroy(&m);
}

/* ---- cnd: signal / wait, broadcast ---- */
static mtx_t cnd_mtx;
static cnd_t cnd_cv;
static int cnd_ready;

static int waiter_fn(void *arg) {
  int *seen = (int *)arg;
  mtx_lock(&cnd_mtx);
  while (!cnd_ready)
    cnd_wait(&cnd_cv, &cnd_mtx);
  *seen = 1;
  mtx_unlock(&cnd_mtx);
  return 0;
}

void test_cnd_signal(void) {
  mtx_init(&cnd_mtx, mtx_plain);
  cnd_init(&cnd_cv);
  cnd_ready = 0;
  int seen = 0;
  thrd_t t;
  TEST_ASSERT_EQUAL_INT(thrd_success, thrd_create(&t, waiter_fn, &seen));
  /* let waiter block on the cv */
  thrd_sleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000},
             NULL);
  mtx_lock(&cnd_mtx);
  cnd_ready = 1;
  cnd_signal(&cnd_cv);
  mtx_unlock(&cnd_mtx);
  TEST_ASSERT_EQUAL_INT(thrd_success, thrd_join(t, NULL));
  TEST_ASSERT_EQUAL_INT(1, seen);
  cnd_destroy(&cnd_cv);
  mtx_destroy(&cnd_mtx);
}

/* ---- call_once ---- */
static once_flag once_flag_var = ONCE_FLAG_INIT;
static int once_count;
static mtx_t once_mtx;

static void once_init_fn(void) {
  mtx_lock(&once_mtx);
  once_count++;
  mtx_unlock(&once_mtx);
}

static int once_caller_fn(void *arg) {
  (void)arg;
  call_once(&once_flag_var, once_init_fn);
  return 0;
}

void test_call_once(void) {
  mtx_init(&once_mtx, mtx_plain);
  once_count = 0;
  enum { N = 8 };
  thrd_t ts[N];
  for (int i = 0; i < N; i++)
    TEST_ASSERT_EQUAL_INT(thrd_success,
                          thrd_create(&ts[i], once_caller_fn, NULL));
  for (int i = 0; i < N; i++)
    TEST_ASSERT_EQUAL_INT(thrd_success, thrd_join(ts[i], NULL));
  TEST_ASSERT_EQUAL_INT(1, once_count);
  mtx_destroy(&once_mtx);
}

/* ---- tss: thread-specific storage ---- */
static tss_t tss_key;
static int tss_dtor_ran;
static void tss_dtor(void *p) {
  (void)p;
  tss_dtor_ran = 1;
}

static int tss_thread_fn(void *arg) {
  int *mine = (int *)arg;
  tss_set(tss_key, mine);
  int *got = (int *)tss_get(tss_key);
  TEST_ASSERT_EQUAL_PTR(mine, got);
  *mine = 123;
  return 0;
}

void test_tss(void) {
  TEST_ASSERT_EQUAL_INT(thrd_success, tss_create(&tss_key, tss_dtor));
  thrd_t t;
  int val = 0;
  tss_dtor_ran = 0;
  TEST_ASSERT_EQUAL_INT(thrd_success, thrd_create(&t, tss_thread_fn, &val));
  TEST_ASSERT_EQUAL_INT(thrd_success, thrd_join(t, NULL));
  TEST_ASSERT_EQUAL_INT(123, val);
  TEST_ASSERT_EQUAL_INT(1, tss_dtor_ran);
  tss_delete(tss_key);
}

/* ---- thrd_yield / thrd_current ---- */
static int current_reporter_fn(void *arg) {
  *(thrd_t *)arg = thrd_current();
  return 0;
}

void test_thrd_current_self(void) {
  thrd_t me = thrd_current();
  thrd_t child;
  thrd_t child_self = me;
  TEST_ASSERT_EQUAL_INT(thrd_success,
                        thrd_create(&child, current_reporter_fn, &child_self));
  TEST_ASSERT_EQUAL_INT(thrd_success, thrd_join(child, NULL));
  /* thrd_equal returns non-zero when equal (C11). The child's self-reported
   * id must match the handle thrd_create returned, and must differ from the
   * spawner's id (each thread has its own TCB at %fs:0). */
  TEST_ASSERT_EQUAL_INT(1, thrd_equal(child_self, child));
  TEST_ASSERT_EQUAL_INT(0, thrd_equal(me, child_self));
  thrd_yield();
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();
  RUN_TEST(test_thrd_create_join);
  RUN_TEST(test_mtx_lock_stress);
  RUN_TEST(test_mtx_recursive);
  RUN_TEST(test_mtx_trylock);
  RUN_TEST(test_cnd_signal);
  RUN_TEST(test_call_once);
  RUN_TEST(test_tss);
  RUN_TEST(test_thrd_current_self);
  return UNITY_END();
}

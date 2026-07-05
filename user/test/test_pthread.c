/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <unity.h>
// sched_yield() 已由 <unistd.h> 声明,无需 <sched.h>;且本 OS 无该头,
// 尖括号会回退到宿主机 /usr/include/sched.h,拉入宿主机 struct timespec
// 与我们的 xos/time.h 重定义冲突。

void setUp(void) {}
void tearDown(void) {}

static void *thread_basic_fn(void *arg) {
  int *p = (int *)arg;
  *p = 42;
  return NULL;
}

void test_pthread_create_join(void) {
  pthread_t t;
  int val = 0;
  TEST_ASSERT_EQUAL_INT(0, pthread_create(&t, NULL, thread_basic_fn, &val));
  TEST_ASSERT_EQUAL_INT(0, pthread_join(t, NULL));
  TEST_ASSERT_EQUAL_INT(42, val);
}

static void *thread_self_fn(void *arg) {
  *(pthread_t *)arg = pthread_self();
  return NULL;
}

void test_pthread_self(void) {
  pthread_t t, self_captured;
  TEST_ASSERT_EQUAL_INT(
      0, pthread_create(&t, NULL, thread_self_fn, &self_captured));
  pthread_join(t, NULL);
  TEST_ASSERT_EQUAL_INT((int)t, (int)self_captured);
}

static pthread_mutex_t test_mutex;
static int counter = 0;

static void *thread_mutex_fn(void *arg) {
  (void)arg;
  for (int i = 0; i < 1000; i++) {
    pthread_mutex_lock(&test_mutex);
    counter++;
    pthread_mutex_unlock(&test_mutex);
  }
  return NULL;
}

void test_pthread_mutex(void) {
  pthread_mutex_init(&test_mutex, NULL);
  counter = 0;
  pthread_t t1, t2;
  TEST_ASSERT_EQUAL_INT(0, pthread_create(&t1, NULL, thread_mutex_fn, NULL));
  TEST_ASSERT_EQUAL_INT(0, pthread_create(&t2, NULL, thread_mutex_fn, NULL));
  pthread_join(t1, NULL);
  pthread_join(t2, NULL);
  TEST_ASSERT_EQUAL_INT(2000, counter);
  pthread_mutex_destroy(&test_mutex);
}

// ---- mutex stress：多线程高竞争 + randomized delay，覆盖三态 mutex 边界 ----
// bug.md Bug 5 的回归测试：手搓三态 mutex 在 contention 标记竞态下会遗留
// state=2 永久自旋。这里用 8 线程 × 5000 次加解锁 + 随机 yield 制造时序，
// 若 mutex slow path 回归会表现为 counter 不等于预期或卡死。
#define STRESS_THREADS 8
#define STRESS_ITERS 5000
static pthread_mutex_t stress_mutex;
static long stress_counter = 0;

static void *thread_stress_fn(void *arg) {
  unsigned int seed = (unsigned int)(uintptr_t)arg ^ 0x9e3779b9u;
  for (int i = 0; i < STRESS_ITERS; i++) {
    pthread_mutex_lock(&stress_mutex);
    stress_counter++;
    pthread_mutex_unlock(&stress_mutex);
    // 伪随机 yield 制造时序抖动，让 fast/slow path 都被覆盖
    seed = seed * 1103515245u + 12345u;
    if ((seed & 0x7) == 0)
      sched_yield();
  }
  return NULL;
}

void test_pthread_mutex_stress(void) {
  pthread_mutex_init(&stress_mutex, NULL);
  stress_counter = 0;
  pthread_t threads[STRESS_THREADS];
  for (int i = 0; i < STRESS_THREADS; i++) {
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&threads[i], NULL, thread_stress_fn,
                                            (void *)(uintptr_t)i));
  }
  for (int i = 0; i < STRESS_THREADS; i++) {
    TEST_ASSERT_EQUAL_INT(0, pthread_join(threads[i], NULL));
  }
  TEST_ASSERT_EQUAL_INT(STRESS_THREADS * STRESS_ITERS, stress_counter);
  pthread_mutex_destroy(&stress_mutex);
}

static pthread_mutex_t cv_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv_cond = PTHREAD_COND_INITIALIZER;
static int cv_ready = 0;

static void *thread_cond_wait_fn(void *arg) {
  (void)arg;
  pthread_mutex_lock(&cv_mutex);
  while (!cv_ready) {
    pthread_cond_wait(&cv_cond, &cv_mutex);
  }
  pthread_mutex_unlock(&cv_mutex);
  return NULL;
}

void test_pthread_cond(void) {
  cv_ready = 0;
  pthread_t t;
  TEST_ASSERT_EQUAL_INT(0, pthread_create(&t, NULL, thread_cond_wait_fn, NULL));
  sched_yield();
  sched_yield();
  pthread_mutex_lock(&cv_mutex);
  cv_ready = 1;
  pthread_cond_signal(&cv_cond);
  pthread_mutex_unlock(&cv_mutex);
  TEST_ASSERT_EQUAL_INT(0, pthread_join(t, NULL));
}

static pthread_barrier_t test_barrier;
static int barrier_count = 0;
static pthread_mutex_t barrier_counter_mutex = PTHREAD_MUTEX_INITIALIZER;

static void *thread_barrier_fn(void *arg) {
  (void)arg;
  pthread_barrier_wait(&test_barrier);
  pthread_mutex_lock(&barrier_counter_mutex);
  barrier_count++;
  pthread_mutex_unlock(&barrier_counter_mutex);
  return NULL;
}

void test_pthread_barrier(void) {
  barrier_count = 0;
  TEST_ASSERT_EQUAL_INT(0, pthread_barrier_init(&test_barrier, NULL, 3));
  pthread_t t1, t2;
  TEST_ASSERT_EQUAL_INT(0, pthread_create(&t1, NULL, thread_barrier_fn, NULL));
  TEST_ASSERT_EQUAL_INT(0, pthread_create(&t2, NULL, thread_barrier_fn, NULL));
  pthread_barrier_wait(&test_barrier);
  pthread_join(t1, NULL);
  pthread_join(t2, NULL);
  TEST_ASSERT_EQUAL_INT(2, barrier_count);
  pthread_barrier_destroy(&test_barrier);
}

static pthread_rwlock_t test_rwlock;
static int rwlock_data = 0;

static void *thread_rwlock_read_fn(void *arg) {
  (void)arg;
  pthread_rwlock_rdlock(&test_rwlock);
  int v = rwlock_data;
  (void)v;
  pthread_rwlock_unlock(&test_rwlock);
  return NULL;
}

void test_pthread_rwlock(void) {
  pthread_rwlock_init(&test_rwlock, NULL);
  rwlock_data = 100;
  pthread_t t1, t2;
  TEST_ASSERT_EQUAL_INT(0,
                        pthread_create(&t1, NULL, thread_rwlock_read_fn, NULL));
  TEST_ASSERT_EQUAL_INT(0,
                        pthread_create(&t2, NULL, thread_rwlock_read_fn, NULL));
  pthread_join(t1, NULL);
  pthread_join(t2, NULL);
  TEST_ASSERT_EQUAL_INT(100, rwlock_data);
  pthread_rwlock_destroy(&test_rwlock);
}

void test_pthread_attr(void) {
  pthread_attr_t attr;
  TEST_ASSERT_EQUAL_INT(0, pthread_attr_init(&attr));
  int ds;
  TEST_ASSERT_EQUAL_INT(0, pthread_attr_getdetachstate(&attr, &ds));
  TEST_ASSERT_EQUAL_INT(PTHREAD_CREATE_JOINABLE, ds);
  TEST_ASSERT_EQUAL_INT(
      0, pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED));
  TEST_ASSERT_EQUAL_INT(0, pthread_attr_getdetachstate(&attr, &ds));
  TEST_ASSERT_EQUAL_INT(PTHREAD_CREATE_DETACHED, ds);
  pthread_attr_destroy(&attr);
}

static void *thread_retval_return_fn(void *arg) {
  (void)arg;
  return (void *)0x1234;
}
static void *thread_retval_exit_fn(void *arg) {
  (void)arg;
  pthread_exit((void *)-1);
}
void test_pthread_retval_return(void) {
  pthread_t t;
  void *ret = NULL;
  TEST_ASSERT_EQUAL_INT(
      0, pthread_create(&t, NULL, thread_retval_return_fn, NULL));
  TEST_ASSERT_EQUAL_INT(0, pthread_join(t, &ret));
  TEST_ASSERT_EQUAL_PTR((void *)0x1234, ret);
}
void test_pthread_retval_exit(void) {
  pthread_t t;
  void *ret = NULL;
  TEST_ASSERT_EQUAL_INT(0,
                        pthread_create(&t, NULL, thread_retval_exit_fn, NULL));
  TEST_ASSERT_EQUAL_INT(0, pthread_join(t, &ret));
  TEST_ASSERT_EQUAL_PTR((void *)-1, ret);
}

static void *thread_main_exit_child_fn(void *arg) {
  (void)arg;
  return NULL;
}
void test_pthread_main_exit_safe(void) {
  pthread_t t;
  TEST_ASSERT_EQUAL_INT(
      0, pthread_create(&t, NULL, thread_main_exit_child_fn, NULL));
  pthread_join(t, NULL);
  // 不调用 pthread_exit，避免终结整个测试进程；NULL-entry 路径由其它用例覆盖。
}

static void *thread_guard_overflow_fn(void *arg) {
  (void)arg;
  void *g = mmap(NULL, 4096, 0, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (g == MAP_FAILED)
    return NULL;
  *(volatile char *)g = 1; // 应触发 #PF
  return NULL;
}
void test_pthread_guard_pf(void) {
  pthread_t t;
  pthread_create(&t, NULL, thread_guard_overflow_fn, NULL);
  pthread_join(t, NULL);
}

static void *thread_detached_fn(void *arg) {
  (void)arg;
  volatile char buf[1024];
  for (int i = 0; i < 1024; i++)
    buf[i] = (char)i;
  return NULL;
}
void test_pthread_detached_leak(void) {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  for (int i = 0; i < 32; i++) {
    pthread_t t;
    TEST_ASSERT_EQUAL_INT(0,
                          pthread_create(&t, &attr, thread_detached_fn, NULL));
    sched_yield();
  }
  pthread_attr_destroy(&attr);
}

static pthread_mutex_t timed_mutex;
static volatile int timed_holder_ready = 0;
static void *thread_timed_holder_fn(void *arg) {
  (void)arg;
  pthread_mutex_lock(&timed_mutex);
  timed_holder_ready = 1; // signal: holder has locked timed_mutex
  struct timespec ts = {0, 50 * 1000000L};
  nanosleep(&ts, NULL);
  pthread_mutex_unlock(&timed_mutex);
  return NULL;
}
void test_pthread_mutex_timedlock_timeout(void) {
  pthread_mutex_init(&timed_mutex, NULL);
  timed_holder_ready = 0;
  pthread_t holder;
  pthread_create(&holder, NULL, thread_timed_holder_fn, NULL);
  while (!timed_holder_ready)
    sched_yield(); // wait until holder has the lock
  struct timespec now;
  timespec_get(&now, TIME_UTC);
  struct timespec abstime;
  abstime.tv_sec = now.tv_sec;
  abstime.tv_nsec = now.tv_nsec + 10 * 1000000L; // +10ms
  if (abstime.tv_nsec >= 1000000000L) {
    abstime.tv_sec += 1;
    abstime.tv_nsec -= 1000000000L;
  }
  int r = pthread_mutex_timedlock(&timed_mutex, &abstime);
  TEST_ASSERT_EQUAL_INT(ETIMEDOUT, r);
  pthread_join(holder, NULL);
  pthread_mutex_destroy(&timed_mutex);
}

static pthread_mutex_t stress_mutex;
static volatile int stress_stop = 0;
static void *thread_futex_stress_fn(void *arg) {
  (void)arg;
  while (!stress_stop) {
    pthread_mutex_lock(&stress_mutex);
    pthread_mutex_unlock(&stress_mutex);
  }
  return NULL;
}
void test_futex_stress(void) {
  pthread_mutex_init(&stress_mutex, NULL);
  pthread_t t[8];
  for (int i = 0; i < 8; i++)
    pthread_create(&t[i], NULL, thread_futex_stress_fn, NULL);
  struct timespec ts = {1, 0};
  nanosleep(&ts, NULL);
  stress_stop = 1;
  for (int i = 0; i < 8; i++)
    pthread_join(t[i], NULL);
  pthread_mutex_destroy(&stress_mutex);
}

static volatile int jctx_b_running = 1;
static void *jctx_thread_b(void *arg) {
  (void)arg;
  while (jctx_b_running)
    sched_yield();
  return NULL;
}
static void *jctx_thread_a(void *arg) {
  pthread_t b = *(pthread_t *)arg;
  pthread_join(b, NULL);
  return NULL;
}
void test_pthread_join_cancel(void) {
  pthread_t b, a;
  pthread_create(&b, NULL, jctx_thread_b, NULL);
  pthread_create(&a, NULL, jctx_thread_a, &b);
  sched_yield();
  sched_yield();
  pthread_cancel(a);
  pthread_join(a, NULL);
  jctx_b_running = 0;
  pthread_join(b, NULL);
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();
  RUN_TEST(test_pthread_create_join);
  RUN_TEST(test_pthread_self);
  RUN_TEST(test_pthread_mutex);
  RUN_TEST(test_pthread_mutex_stress);
  RUN_TEST(test_pthread_cond);
  RUN_TEST(test_pthread_barrier);
  RUN_TEST(test_pthread_rwlock);
  RUN_TEST(test_pthread_attr);
  RUN_TEST(test_pthread_retval_return);
  RUN_TEST(test_pthread_retval_exit);
  RUN_TEST(test_pthread_main_exit_safe);
  RUN_TEST(test_pthread_guard_pf);
  RUN_TEST(test_pthread_detached_leak);
  RUN_TEST(test_pthread_mutex_timedlock_timeout);
  RUN_TEST(test_futex_stress);
  RUN_TEST(test_pthread_join_cancel);
  return UNITY_END();
}

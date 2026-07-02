#include <unity.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sched.h>

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
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&t, NULL, thread_self_fn, &self_captured));
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
#define STRESS_ITERS   5000
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
        if ((seed & 0x7) == 0) sched_yield();
    }
    return NULL;
}

void test_pthread_mutex_stress(void) {
    pthread_mutex_init(&stress_mutex, NULL);
    stress_counter = 0;
    pthread_t threads[STRESS_THREADS];
    for (int i = 0; i < STRESS_THREADS; i++) {
        TEST_ASSERT_EQUAL_INT(0, pthread_create(&threads[i], NULL,
                                                thread_stress_fn,
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
    sched_yield(); sched_yield();
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
    int v = rwlock_data; (void)v;
    pthread_rwlock_unlock(&test_rwlock);
    return NULL;
}

void test_pthread_rwlock(void) {
    pthread_rwlock_init(&test_rwlock, NULL);
    rwlock_data = 100;
    pthread_t t1, t2;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&t1, NULL, thread_rwlock_read_fn, NULL));
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&t2, NULL, thread_rwlock_read_fn, NULL));
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
    TEST_ASSERT_EQUAL_INT(0, pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED));
    TEST_ASSERT_EQUAL_INT(0, pthread_attr_getdetachstate(&attr, &ds));
    TEST_ASSERT_EQUAL_INT(PTHREAD_CREATE_DETACHED, ds);
    pthread_attr_destroy(&attr);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_pthread_create_join);
    RUN_TEST(test_pthread_self);
    RUN_TEST(test_pthread_mutex);
    RUN_TEST(test_pthread_mutex_stress);
    RUN_TEST(test_pthread_cond);
    RUN_TEST(test_pthread_barrier);
    RUN_TEST(test_pthread_rwlock);
    RUN_TEST(test_pthread_attr);
    return UNITY_END();
}

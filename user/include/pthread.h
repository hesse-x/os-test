#ifndef _PTHREAD_H
#define _PTHREAD_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include "common/signal.h"

#ifdef __cplusplus
extern "C" {
#endif

// ===================== Thread types =====================
typedef int32_t pthread_t;
typedef int32_t pthread_key_t;

// ===================== Constants =====================
#define PTHREAD_CREATE_JOINABLE       0
#define PTHREAD_CREATE_DETACHED       1

#define PTHREAD_CANCEL_ENABLE         0
#define PTHREAD_CANCEL_DISABLE        1
#define PTHREAD_CANCEL_DEFERRED       0
#define PTHREAD_CANCEL_ASYNCHRONOUS   1
#define PTHREAD_CANCELED              ((void *)-1)

#define PTHREAD_INHERIT_SCHED         0
#define PTHREAD_EXPLICIT_SCHED        1

#define PTHREAD_MUTEX_NORMAL          0
#define PTHREAD_MUTEX_ERRORCHECK      1
#define PTHREAD_MUTEX_RECURSIVE       2
#define PTHREAD_MUTEX_DEFAULT         PTHREAD_MUTEX_NORMAL

#define PTHREAD_PROCESS_PRIVATE       0
#define PTHREAD_PROCESS_SHARED        1

#define PTHREAD_KEYS_MAX              128
#define PTHREAD_DESTRUCTOR_ITERATIONS 4

#define PTHREAD_ONCE_INIT             0
#define PTHREAD_BARRIER_SERIAL_THREAD -1

// ===================== Mutex =====================
typedef struct {
    uint32_t state;
    uint32_t type;
    uint32_t owner;
    uint32_t count;
} pthread_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER {0, PTHREAD_MUTEX_NORMAL, 0, 0}

typedef struct {
    uint32_t type;
} pthread_mutexattr_t;

// ===================== Condition variable =====================
typedef struct {
    uint32_t seq;
    uint32_t waiters;
} pthread_cond_t;

#define PTHREAD_COND_INITIALIZER {0, 0}

typedef struct {
    uint32_t dummy;
} pthread_condattr_t;

// ===================== Read-write lock =====================
typedef struct {
    uint32_t readers;
    uint32_t writer;
    uint32_t wwaiters;
    uint32_t rwaiters;
} pthread_rwlock_t;

#define PTHREAD_RWLOCK_INITIALIZER {0, 0, 0, 0}

typedef struct {
    uint32_t dummy;
} pthread_rwlockattr_t;

// ===================== Barrier =====================
typedef struct {
    uint32_t count;
    uint32_t waiting;
    uint32_t generation;
} pthread_barrier_t;

typedef struct {
    uint32_t dummy;
} pthread_barrierattr_t;

// ===================== Once =====================
typedef struct {
    uint32_t done;
} pthread_once_t;

// ===================== Attributes =====================
typedef struct {
    int    detachstate;
    void  *stack;
    size_t stacksize;
    size_t guardsize;
} pthread_attr_t;

#define PTHREAD_STACK_MIN    16384
#define PTHREAD_STACK_DEFAULT (64 * 1024)

// ===================== Cleanup handler =====================
typedef struct __pthread_cleanup_handler {
    void (*routine)(void *);
    void *arg;
    struct __pthread_cleanup_handler *prev;
} __pthread_cleanup_handler_t;

// ===================== Function declarations =====================
int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg);
void pthread_exit(void *retval) __attribute__((noreturn));
int pthread_join(pthread_t thread, void **retval);
int pthread_detach(pthread_t thread);
pthread_t pthread_self(void);
int pthread_equal(pthread_t t1, pthread_t t2);
int pthread_cancel(pthread_t thread);
int pthread_setcancelstate(int state, int *oldstate);
int pthread_setcanceltype(int type, int *oldtype);
void pthread_testcancel(void);

int pthread_attr_init(pthread_attr_t *attr);
int pthread_attr_destroy(pthread_attr_t *attr);
int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *detachstate);
int pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate);
int pthread_attr_getstack(const pthread_attr_t *attr, void **stackaddr, size_t *stacksize);
int pthread_attr_setstack(pthread_attr_t *attr, void *stackaddr, size_t stacksize);
int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *stacksize);
int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize);
int pthread_attr_getguardsize(const pthread_attr_t *attr, size_t *guardsize);
int pthread_attr_setguardsize(pthread_attr_t *attr, size_t guardsize);

void pthread_cleanup_push(void (*routine)(void *), void *arg);
void pthread_cleanup_pop(int execute);

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_trylock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);
int pthread_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *abstime);
int pthread_mutexattr_init(pthread_mutexattr_t *attr);
int pthread_mutexattr_destroy(pthread_mutexattr_t *attr);
int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *type);
int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type);

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr);
int pthread_cond_destroy(pthread_cond_t *cond);
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int pthread_cond_signal(pthread_cond_t *cond);
int pthread_cond_broadcast(pthread_cond_t *cond);

int pthread_rwlock_init(pthread_rwlock_t *rwlock, const pthread_rwlockattr_t *attr);
int pthread_rwlock_destroy(pthread_rwlock_t *rwlock);
int pthread_rwlock_rdlock(pthread_rwlock_t *rwlock);
int pthread_rwlock_wrlock(pthread_rwlock_t *rwlock);
int pthread_rwlock_unlock(pthread_rwlock_t *rwlock);

int pthread_barrier_init(pthread_barrier_t *barrier, const pthread_barrierattr_t *attr,
                         unsigned count);
int pthread_barrier_destroy(pthread_barrier_t *barrier);
int pthread_barrier_wait(pthread_barrier_t *barrier);

int pthread_once(pthread_once_t *once, void (*init)(void));

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *));
int pthread_setspecific(pthread_key_t key, const void *value);
void *pthread_getspecific(pthread_key_t key);

int pthread_kill(pthread_t thread, int sig);
int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset);
int pthread_setname_np(pthread_t thread, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* _PTHREAD_H */

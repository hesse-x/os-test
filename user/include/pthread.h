#ifndef _PTHREAD_H
#define _PTHREAD_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// ===================== pthread types =====================

typedef unsigned long pthread_t;

// Mutex
typedef struct {
    int __lock;          // 0=unlocked, 1=locked
    unsigned int __type; // PTHREAD_MUTEX_NORMAL etc.
} pthread_mutex_t;

// Mutex attributes (minimal)
typedef struct {
    int __type;
} pthread_mutexattr_t;

// Condition variable
typedef struct {
    int __seq;           // broadcast counter
} pthread_cond_t;

// Condition variable attributes (minimal)
typedef struct {
    int __dummy;
} pthread_condattr_t;

// ===================== Mutex constants =====================

#define PTHREAD_MUTEX_NORMAL     0
#define PTHREAD_MUTEX_ERRORCHECK 1
#define PTHREAD_MUTEX_RECURSIVE  2

#define PTHREAD_MUTEX_DEFAULT    PTHREAD_MUTEX_NORMAL

// ===================== Mutex initializers =====================

#define PTHREAD_MUTEX_INITIALIZER    { 0, PTHREAD_MUTEX_NORMAL }
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER { 0, PTHREAD_MUTEX_RECURSIVE }

// ===================== Condition initializers =====================

#define PTHREAD_COND_INITIALIZER    { 0 }

// ===================== Thread functions =====================

int pthread_create(pthread_t *thread, const void *attr,
                   void *(*start_routine)(void *), void *arg);
int pthread_join(pthread_t thread, void **retval);
void pthread_exit(void *retval);
pthread_t pthread_self(void);

// ===================== Mutex functions =====================

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);
int pthread_mutex_destroy(pthread_mutex_t *mutex);

// ===================== Condition variable functions =====================

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr);
int pthread_cond_signal(pthread_cond_t *cond);
int pthread_cond_broadcast(pthread_cond_t *cond);
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int pthread_cond_destroy(pthread_cond_t *cond);

// ===================== TID =====================

pid_t gettid(void);

#ifdef __cplusplus
}
#endif

#endif /* _PTHREAD_H */

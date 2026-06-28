#ifndef KERNEL_ATOMIC_H
#define KERNEL_ATOMIC_H

#include <stdint.h>
#include <stdbool.h>
#include "kernel/log.h"   // BUG_ON

// ===================== atomic_t =====================
typedef struct { int counter; } atomic_t;

static inline int atomic_read(atomic_t *v) {
    return __atomic_load_n(&v->counter, __ATOMIC_ACQUIRE);
}

static inline void atomic_set(atomic_t *v, int i) {
    __atomic_store_n(&v->counter, i, __ATOMIC_RELEASE);
}

static inline int atomic_add_return(atomic_t *v, int i) {
    return __atomic_add_fetch(&v->counter, i, __ATOMIC_ACQ_REL);
}

static inline int atomic_sub_return(atomic_t *v, int i) {
    return __atomic_sub_fetch(&v->counter, i, __ATOMIC_ACQ_REL);
}

static inline int atomic_inc_return(atomic_t *v) {
    return atomic_add_return(v, 1);
}

static inline int atomic_dec_return(atomic_t *v) {
    return atomic_sub_return(v, 1);
}

static inline bool atomic_dec_and_test(atomic_t *v) {
    return atomic_sub_return(v, 1) == 0;
}

// ===================== refcount_t =====================
// 0 = free, >0 = in-use, BUG_ON underflow
typedef struct { atomic_t refs; } refcount_t;

static inline void refcount_set(refcount_t *r, int n) {
    atomic_set(&r->refs, n);
}

static inline int refcount_read(refcount_t *r) {
    return atomic_read(&r->refs);
}

static inline void refcount_inc(refcount_t *r) {
    int old = atomic_inc_return(&r->refs);
    BUG_ON(old <= 0);
}

static inline bool refcount_dec_and_test(refcount_t *r) {
    int old = atomic_dec_return(&r->refs);
    BUG_ON(old < 0);
    return old == 1;
}

#endif /* KERNEL_ATOMIC_H */

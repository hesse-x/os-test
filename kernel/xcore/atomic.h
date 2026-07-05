/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_ATOMIC_H
#define KERNEL_ATOMIC_H

#include "kernel/xcore/log.h" // BUG_ON
#include <stdbool.h>
#include <stdint.h>

// ===================== atomic_t =====================
typedef struct {
  int counter;
} atomic_t;

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

// Convenience: increment without caring about return value
static inline void atomic_inc(atomic_t *v) { (void)atomic_add_return(v, 1); }
// Convenience: decrement without caring about return value
static inline void atomic_dec(atomic_t *v) { (void)atomic_sub_return(v, 1); }

// ===================== refcount_t =====================
// 0 = free, >0 = in-use, BUG_ON underflow
typedef struct {
  atomic_t refs;
} refcount_t;

static inline void refcount_set(refcount_t *r, int n) {
  atomic_set(&r->refs, n);
}

static inline int refcount_read(refcount_t *r) { return atomic_read(&r->refs); }

static inline void refcount_inc(refcount_t *r) {
  // atomic_inc_return returns new value; old value = new - 1
  // old ≤ 0 = UAF (incrementing from a freed object)
  BUG_ON(atomic_inc_return(&r->refs) <= 1);
}

static inline bool refcount_dec_and_test(refcount_t *r) {
  int new_val = atomic_dec_return(&r->refs);
  BUG_ON(new_val < 0);
  return new_val == 0;
}

#endif /* KERNEL_ATOMIC_H */

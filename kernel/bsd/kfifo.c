/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#include "kernel/bsd/kfifo.h"

#include "arch/x64/utils.h"   // __memcpy
#include "kernel/xcore/kpi.h" // kmalloc/kfree
#include <stddef.h>

bool kfifo_alloc(kfifo *kf, uint32_t cap, uint32_t esize) {
  if (!kf || cap == 0 || esize == 0)
    return false;
  kf->buf = kmalloc((size_t)cap * esize);
  if (!kf->buf)
    return false;
  kf->cap = cap;
  kf->esize = esize;
  kf->head = 0;
  kf->tail = 0;
  return true;
}

void kfifo_free(kfifo *kf) {
  if (!kf || !kf->buf)
    return;
  kfree(kf->buf);
  kf->buf = NULL;
}

bool kfifo_in(kfifo *kf, const void *elem) {
  if (!kf || !kf->buf)
    return false;
  uint32_t h = __atomic_load_n(&kf->head, __ATOMIC_ACQUIRE);
  uint32_t t = kf->tail;
  uint32_t next = (t + 1) % kf->cap;
  if (next == h) // 满
    return false;
  __memcpy((uint8_t *)kf->buf + (size_t)t * kf->esize, elem, kf->esize);
  __atomic_store_n(&kf->tail, next, __ATOMIC_RELEASE);
  return true;
}

uint32_t kfifo_in_batch(kfifo *kf, const void *elems, uint32_t count) {
  if (!kf || !kf->buf || count == 0)
    return 0;
  uint32_t done = 0;
  const uint8_t *p = (const uint8_t *)elems;
  for (uint32_t i = 0; i < count; i++) {
    if (!kfifo_in(kf, p + (size_t)i * kf->esize))
      break;
    done++;
  }
  return done;
}

bool kfifo_out(kfifo *kf, void *out) {
  if (!kf || !kf->buf)
    return false;
  uint32_t h = kf->head;
  uint32_t t = __atomic_load_n(&kf->tail, __ATOMIC_ACQUIRE);
  if (h == t) // 空
    return false;
  __memcpy(out, (uint8_t *)kf->buf + (size_t)h * kf->esize, kf->esize);
  __atomic_store_n(&kf->head, (h + 1) % kf->cap, __ATOMIC_RELEASE);
  return true;
}

uint32_t kfifo_out_batch(kfifo *kf, void *out, uint32_t count) {
  if (!kf || !kf->buf || count == 0)
    return 0;
  uint32_t done = 0;
  uint8_t *p = (uint8_t *)out;
  for (uint32_t i = 0; i < count; i++) {
    if (!kfifo_out(kf, p + (size_t)i * kf->esize))
      break;
    done++;
  }
  return done;
}

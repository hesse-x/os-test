/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// kfifo — fixed-size-element single-producer/single-consumer lockless ring.
// Used only for evdev broker per-client event buffers (element = input_event,
// 24B). SPSC: tail has a single writer, head a single reader; head/tail use
// atomic load/store, no lock needed.
#ifndef KERNEL_BSD_KFIFO_H
#define KERNEL_BSD_KFIFO_H

#include <stdbool.h>
#include <stdint.h>

typedef struct kfifo {
  void *buf;      // kmalloc'd element array, capacity = cap * esize
  uint32_t cap;   // number of slots
  uint32_t esize; // element size in bytes
  uint32_t head;  // consumer read position (reader advances)
  uint32_t tail;  // producer write position (writer advances)
} kfifo;

// Allocate a ring holding `cap` elements of `esize` bytes. false on failure.
bool kfifo_alloc(kfifo *kf, uint32_t cap, uint32_t esize);

// Free the ring buffer. Caller ensures no concurrent access afterwards.
void kfifo_free(kfifo *kf);

// Return the current number of available elements (head/tail difference mod
// cap).
static inline uint32_t kfifo_len(const kfifo *kf) {
  uint32_t h = __atomic_load_n(&kf->head, __ATOMIC_ACQUIRE);
  uint32_t t = __atomic_load_n(&kf->tail, __ATOMIC_ACQUIRE);
  return (t >= h) ? (t - h) : (kf->cap - h + t);
}

// Producer: enqueue one element. false when full (caller then drop-new + set
// `dropped`).
bool kfifo_in(kfifo *kf, const void *elem);

// Producer: batch-enqueue `count` elements, return number actually enqueued
// (truncated on full). Does not inject SYN_DROPPED; frame semantics on full are
// handled by the caller (broker write).
uint32_t kfifo_in_batch(kfifo *kf, const void *elems, uint32_t count);

// Consumer: dequeue one element into `out`. false when empty.
bool kfifo_out(kfifo *kf, void *out);

// Consumer: dequeue up to `count` elements into `out`, return actual count.
uint32_t kfifo_out_batch(kfifo *kf, void *out, uint32_t count);

#endif // KERNEL_BSD_KFIFO_H

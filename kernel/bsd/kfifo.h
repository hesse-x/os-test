/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * kfifo — 定长元素的单生产者单消费者无锁环。仅用于 evdev broker 的
 * per-client 事件缓冲（元素 = input_event, 24B）。SPSC：tail 单写者、head
 * 单读者，head/tail 用原子载入/存储，无需锁。
 */
#ifndef KERNEL_BSD_KFIFO_H
#define KERNEL_BSD_KFIFO_H

#include <stdbool.h>
#include <stdint.h>

typedef struct kfifo {
  void *buf;      // kmalloc'd 元素数组，容量 = cap * esize
  uint32_t cap;   // 槽数
  uint32_t esize; // 元素字节数
  uint32_t head;  // 消费者读位置（读者推进）
  uint32_t tail;  // 生产者写位置（写者推进）
} kfifo;

/* 分配容量为 cap 个 esize 字节元素的环。失败返回 false。 */
bool kfifo_alloc(kfifo *kf, uint32_t cap, uint32_t esize);

/* 释放环缓冲。调用方保证此后无并发访问。 */
void kfifo_free(kfifo *kf);

/* 返回当前可用元素数（head 与 tail 之差，模 cap）。 */
static inline uint32_t kfifo_len(const kfifo *kf) {
  uint32_t h = __atomic_load_n(&kf->head, __ATOMIC_ACQUIRE);
  uint32_t t = __atomic_load_n(&kf->tail, __ATOMIC_ACQUIRE);
  return (t >= h) ? (t - h) : (kf->cap - h + t);
}

/* 生产者入队一个元素。环满返回 false（调用方据此 drop-new + 置 dropped）。 */
bool kfifo_in(kfifo *kf, const void *elem);

/* 生产者批量入队 count 个元素，返回实际入队数（环满截断）。不注入
 * SYN_DROPPED；帧语义由调用方（broker write）在满时处理。 */
uint32_t kfifo_in_batch(kfifo *kf, const void *elems, uint32_t count);

/* 消费者出队一个元素到 out。空返回 false。 */
bool kfifo_out(kfifo *kf, void *out);

/* 消费者批量出队最多 count 个元素到 out，返回实际出队数。 */
uint32_t kfifo_out_batch(kfifo *kf, void *out, uint32_t count);

#endif /* KERNEL_BSD_KFIFO_H */

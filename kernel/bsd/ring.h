/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * ring_t — kernel-private ring buffer consumer encapsulation.
 *
 * Operates on a shared ringbuf_header (UAPI, include/uapi/xos/input.h) backed
 * by a struct shm page. The producer (user-space evdev) writes slot[head] and
 * advances head unconditionally (overwrite-oldest policy); it never reads any
 * consumer cursor. Each consumer fd carries its own read cursor in f->offset,
 * so multiple consumers read independently.
 *
 * This header is kernel-internal: the producer side has no ring_t — it uses
 * the user-space ringbuf_push() helper (user/include/xos/ringbuf.h). ring_t
 * only consolidates the kernel read path (read/poll) that ringbuf_fops used
 * to open-code per operation.
 */
#ifndef KERNEL_BSD_RING_H
#define KERNEL_BSD_RING_H

#include <stdint.h>

#include "kernel/bsd/devtmpfs.h" /* __poll, struct file */

struct shm;
struct file;

/* A ring buffer view: the SHM-backed header + precomputed data-area base. */
typedef struct ring_t {
  struct ringbuf_header
      *hdr;      /* SHM offset 0, validated against RINGBUF_MAGIC */
  uint8_t *data; /* = (uint8_t*)hdr + hdr->data_offset */
} ring_t;

/*
 * Build a ring view from a SHM page. Returns a ring_t whose hdr is NULL if the
 * SHM is missing or its magic does not match RINGBUF_MAGIC (caller treats NULL
 * hdr as -ENODEV). Holds no reference of its own — the caller's inode/file
 * keeps the SHM alive for the ring view's lifetime.
 */
ring_t ring_from_shm(struct shm *shm);

/*
 * Read up to count bytes of events into buf, starting at this fd's cursor
 * (f->offset). Advances f->offset. If the producer has lapped this reader
 * (head more than a full turn ahead of the cursor), the cursor jumps to head
 * so the reader sees the newest events rather than stale overwritten slots.
 * When the cursor is caught up to head and O_NONBLOCK is not set, the call
 * blocks on the file's wait-queue until a RINGBUF_WAKE wakes it, then retries;
 * with O_NONBLOCK it returns -EAGAIN instead. count==0 returns 0 immediately.
 * Returns the byte count read, 0 if nothing new, or a negative errno.
 */
ssize_t ring_read(ring_t *r, struct file *f, void *buf, size_t count);

/*
 * Report readiness: POLLIN when this fd's cursor has unread events
 * (f->offset != hdr->head). The fd's wait-queue registration (so a later
 * RINGBUF_WAKE __wake_up reaches this poll) is handled by the sys_poll core,
 * not here.
 */
__poll ring_poll(ring_t *r, struct file *f, int events);

#endif /* KERNEL_BSD_RING_H */

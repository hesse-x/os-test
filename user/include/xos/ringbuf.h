/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * ringbuf_push — user-space producer helper for the SHM event ring.
 *
 * Policy: overwrite-oldest. The producer writes slot[head] and advances head
 * unconditionally; it never reads any consumer cursor and there is no
 * "full" check, so push never fails and never blocks. This matches Linux
 * kernel evdev semantics: input is current-state, not history; a slow
 * consumer is lapped and (via the kernel ring_read slow-reader jump) sees
 * the newest events rather than stale slots.
 *
 * The matching kernel consumer side is ring_t (kernel/bsd/ring.h), which
 * reads via per-fd f->offset cursors. The shared header layout
 * (ringbuf_header, no tail) is defined in include/uapi/xos/input.h.
 */
#ifndef USER_XOS_RINGBUF_H
#define USER_XOS_RINGBUF_H

#include <stdint.h>
#include <xos/input.h> /* ringbuf_header, input_event */

/* Append ev to the ring, overwriting the oldest slot if necessary. */
static inline void ringbuf_push(volatile ringbuf_header *hdr,
                                const input_event *ev) {
  uint32_t head = __atomic_load_n(&hdr->head, __ATOMIC_ACQUIRE);
  volatile input_event *slot =
      (volatile input_event *)((volatile uint8_t *)hdr + hdr->data_offset +
                               head * hdr->elem_size);
  /* Field-wise store: input_event's implicit copy-assign is non-volatile, so a
   * struct assignment would discard the volatile qualifier under -fpermissive.
   */
  slot->tv_sec = ev->tv_sec;
  slot->tv_usec = ev->tv_usec;
  slot->type = ev->type;
  slot->code = ev->code;
  slot->value = ev->value;
  __atomic_store_n(&hdr->head, (head + 1) % hdr->capacity, __ATOMIC_RELEASE);
}

#endif /* USER_XOS_RINGBUF_H */

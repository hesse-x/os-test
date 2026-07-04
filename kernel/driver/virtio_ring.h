/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_DRIVER_VIRTIO_RING_H
#define KERNEL_DRIVER_VIRTIO_RING_H

#include <stdbool.h>
#include <stdint.h>

/* ===== split virtqueue ring sizes ===== */
#define VRING_DESC_F_NEXT 0x01
#define VRING_DESC_F_WRITE 0x02
#define VRING_DESC_F_INDIRECT 0x04

#define VRING_AVAIL_F_NO_INTERRUPT 0x01
#define VRING_USED_F_NO_NOTIFY 0x01

/* ===== split ring descriptor formats ===== */
struct vring_desc {
  uint64_t addr;  /* guest physical address of buffer */
  uint32_t len;   /* buffer length */
  uint16_t flags; /* VRING_DESC_F_* */
  uint16_t next;  /* next desc index (if F_NEXT) */
};

struct vring_avail {
  uint16_t flags;
  uint16_t idx;    /* incremented by producer for each new entry */
  uint16_t ring[]; /* array of desc indices, size = queue_size */
};

struct vring_used_elem {
  uint32_t id;  /* desc index that was used */
  uint32_t len; /* bytes written */
};

struct vring_used {
  uint16_t flags;
  uint16_t idx; /* incremented by device for each used entry */
  struct vring_used_elem ring[];
};

/* ===== virtqueue handle ===== */
struct virtqueue {
  uint16_t index;      /* queue index (for queue_select) */
  uint16_t size;       /* queue size (power of 2) */
  uint16_t notify_off; /* notify offset for this queue */

  /* ring pointers (kernel virtual) */
  struct vring_desc *desc;
  struct vring_avail *avail;
  struct vring_used *used;

  /* free descriptor management */
  uint16_t free_head;  /* index of next free desc */
  uint16_t free_cnt;   /* number of free descs */
  uint16_t *next_free; /* free chain: next_free[i] = next free desc after i */

  /* producer/consumer indices */
  uint16_t avail_idx; /* next avail index to fill (shadow of avail->idx) */
  uint16_t used_idx;  /* next used index to consume (shadow of used->idx) */

  /* physical address of rings (for telling device via common cfg) */
  uint64_t desc_phys;
  uint64_t avail_phys;
  uint64_t used_phys;

  /* callback for each completed desc */
  void (*callback)(void *ctx, uint32_t len);
  void **ctx; /* ctx[i] = callback context for desc i */

  /* MSI-X vector assigned */
  int msix_vector;
};

/* ===== API ===== */
/* Allocate and initialize a split virtqueue of given size.
   Returns 0 on success. vq pointer filled in. */
int vring_create(struct virtqueue *vq, uint16_t index, uint16_t size,
                 uint16_t notify_off);

/* Free a virtqueue's allocated memory. */
void vring_destroy(struct virtqueue *vq);

/* Allocate one descriptor from the free list. Returns desc index or -1 if full.
 */
int vring_alloc_desc(struct virtqueue *vq);

/* Free a descriptor (chain head) back to the free list. */
void vring_free_desc(struct virtqueue *vq, int idx);

/* Enqueue a buffer described by an array of (addr, len, flags) tuples.
   Returns the head desc index or -1 if no space. */
int vring_add_buf(struct virtqueue *vq, uint64_t *addrs, uint32_t *lens,
                  uint16_t *flags, int count, void *ctx);

/* Make added buffers visible to device (update avail->idx). */
void vring_kick(struct virtqueue *vq);

/* Process used ring: for each used element, call callback and free desc.
   Returns number of completed buffers. */
int vring_poll_used(struct virtqueue *vq);

/* Check if the virtqueue has pending used entries to consume. */
bool vring_has_used(struct virtqueue *vq);

#endif /* KERNEL_DRIVER_VIRTIO_RING_H */

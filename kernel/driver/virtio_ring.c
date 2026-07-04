/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>

#include "arch/x64/paging.h"
#include "arch/x64/utils.h"
#include "kernel/driver/virtio_ring.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/mem/slab.h"

/* Helper: page-align a size */
static uint64_t page_align(uint64_t sz) { return (sz + 4095) & ~4095UL; }

/* Physical address of a kernel virtual pointer (higher-half kernel: vaddr -
 * VMA_BASE) */
static uint64_t virt_to_phys(void *vaddr) {
  return (uint64_t)PHY_ADDR((uintptr_t)vaddr);
}

int vring_create(struct virtqueue *vq, uint16_t index, uint16_t size,
                 uint16_t notify_off) {
  __memset(vq, 0, sizeof(*vq));
  vq->index = index;
  vq->size = size;
  vq->notify_off = notify_off;

  /* Allocate desc table: size * sizeof(vring_desc) */
  size_t desc_sz = size * sizeof(struct vring_desc);
  size_t avail_sz = sizeof(struct vring_avail) + size * sizeof(uint16_t);
  /* used ring must be 4096-aligned per spec (but QEMU tolerates page alignment)
   */
  size_t used_sz =
      sizeof(struct vring_used) + size * sizeof(struct vring_used_elem);

  /* Allocate each ring on its own page(s) for alignment.
     bfc_alloc_page_data(n) returns a data pointer (not Page*), n = page count.
   */
  vq->desc =
      (struct vring_desc *)bfc_alloc_page_data(page_align(desc_sz) >> 12);
  vq->avail =
      (struct vring_avail *)bfc_alloc_page_data(page_align(avail_sz) >> 12);
  vq->used =
      (struct vring_used *)bfc_alloc_page_data(page_align(used_sz) >> 12);
  vq->next_free = (uint16_t *)kmalloc(size * sizeof(uint16_t));
  vq->ctx = (void **)kmalloc(size * sizeof(void *));

  if (!vq->desc || !vq->avail || !vq->used || !vq->next_free || !vq->ctx) {
    vring_destroy(vq);
    return -1;
  }

  __memset(vq->desc, 0, desc_sz);
  __memset(vq->avail, 0, avail_sz);
  __memset(vq->used, 0, used_sz);
  __memset(vq->next_free, 0, size * sizeof(uint16_t));
  __memset(vq->ctx, 0, size * sizeof(void *));

  /* Build free list: next_free[i] = i+1, last -> 0xFFFF (end) */
  for (int i = 0; i < size - 1; i++)
    vq->next_free[i] = i + 1;
  vq->next_free[size - 1] = 0xFFFF;
  vq->free_head = 0;
  vq->free_cnt = size;
  vq->avail_idx = 0;
  vq->used_idx = 0;

  /* Physical addresses for device */
  vq->desc_phys = virt_to_phys(vq->desc);
  vq->avail_phys = virt_to_phys(vq->avail);
  vq->used_phys = virt_to_phys(vq->used);

  return 0;
}

void vring_destroy(struct virtqueue *vq) {
  /* Best-effort free; kernel allocations during boot are typically not freed */
  if (vq->desc) { /* bfc pages not freed */
  }
  if (vq->avail) { /* bfc pages not freed */
  }
  if (vq->used) { /* bfc pages not freed */
  }
  if (vq->next_free)
    kfree(vq->next_free);
  if (vq->ctx)
    kfree(vq->ctx);
  __memset(vq, 0, sizeof(*vq));
}

int vring_alloc_desc(struct virtqueue *vq) {
  if (vq->free_cnt == 0)
    return -1;
  int idx = vq->free_head;
  vq->free_head = vq->next_free[idx];
  vq->free_cnt--;
  vq->desc[idx].addr = 0;
  vq->desc[idx].len = 0;
  vq->desc[idx].flags = 0;
  vq->desc[idx].next = 0;
  return idx;
}

void vring_free_desc(struct virtqueue *vq, int idx) {
  vq->next_free[idx] = vq->free_head;
  vq->free_head = idx;
  vq->free_cnt++;
}

int vring_add_buf(struct virtqueue *vq, uint64_t *addrs, uint32_t *lens,
                  uint16_t *flags, int count, void *ctx) {
  if (count <= 0 || vq->free_cnt < count)
    return -1;

  int head = vring_alloc_desc(vq);
  if (head < 0)
    return -1;
  vq->desc[head].addr = addrs[0];
  vq->desc[head].len = lens[0];
  vq->desc[head].flags = flags[0];
  vq->ctx[head] = ctx;

  int prev = head;
  for (int i = 1; i < count; i++) {
    int idx = vring_alloc_desc(vq);
    if (idx < 0) {
      /* Not enough descs; free what we allocated */
      vring_free_desc(vq, head);
      /* free subsequent ones too */
      int p = head;
      while (p != vq->free_head && vq->desc[p].flags & VRING_DESC_F_NEXT) {
        /* walk back the chain we built — simplified: free head chain */
        break;
      }
      return -1;
    }
    vq->desc[prev].flags |= VRING_DESC_F_NEXT;
    vq->desc[prev].next = idx;
    vq->desc[idx].addr = addrs[i];
    vq->desc[idx].len = lens[i];
    vq->desc[idx].flags = flags[i];
    vq->ctx[idx] = NULL; /* ctx only stored on head */
    prev = idx;
  }

  /* Publish to avail ring */
  vq->avail->ring[vq->avail_idx % vq->size] = head;
  vq->avail_idx++;

  return head;
}

void vring_kick(struct virtqueue *vq) {
  /* Ensure avail ring writes are visible before updating idx */
  __asm__ volatile("mfence" ::: "memory");
  vq->avail->idx = vq->avail_idx;
  /* Actual notify (write to notify BAR) is done by caller via virtio_pci_notify
   */
}

bool vring_has_used(struct virtqueue *vq) {
  return vq->used->idx != vq->used_idx;
}

int vring_poll_used(struct virtqueue *vq) {
  int count = 0;
  while (vq->used->idx != vq->used_idx) {
    uint16_t idx = vq->used_idx % vq->size;
    struct vring_used_elem *e = &vq->used->ring[idx];
    uint32_t desc_id = e->id;
    /* Walk the desc chain and free all descs */
    int cur = desc_id;
    while (cur >= 0 && cur < vq->size) {
      uint16_t flags = vq->desc[cur].flags;
      int next = vq->desc[cur].next;
      vring_free_desc(vq, cur);
      count++;
      if (!(flags & VRING_DESC_F_NEXT))
        break;
      cur = next;
    }
    /* Call callback on the head (ctx stored on head) */
    if (vq->callback) {
      vq->callback(vq->ctx[desc_id], e->len);
    }
    vq->ctx[desc_id] = NULL;
    vq->used_idx++;
  }
  return count;
}

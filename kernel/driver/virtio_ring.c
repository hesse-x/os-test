/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>

#include "arch/x64/paging.h"
#include "arch/x64/utils.h"
#include "kernel/driver/virtio_ring.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/mem/slab.h"

#include <xos/errno.h>

/* Helper: page-align a size */
static bool checked_ring_size(uint16_t size, size_t elem, size_t base,
                              size_t *bytes, uint16_t *pages) {
  if (!size || size > VRING_MAX_SIZE || (size & (size - 1)) ||
      elem > (SIZE_MAX - base) / size)
    return false;
  *bytes = base + (size_t)size * elem;
  if (*bytes > SIZE_MAX - 4095)
    return false;
  size_t page_count = (*bytes + 4095) >> 12;
  if (!page_count || page_count > UINT16_MAX)
    return false;
  *pages = (uint16_t)page_count;
  return true;
}

/* Physical address of a kernel virtual pointer (higher-half kernel: vaddr -
 * VMA_BASE) */
static uint64_t virt_to_phys(void *vaddr) {
  return (uint64_t)PHY_ADDR((uintptr_t)vaddr);
}

int vring_create(struct virtqueue *vq, uint16_t index, uint16_t size,
                 uint16_t notify_off) {
  if (!vq)
    return -EINVAL;
  __memset(vq, 0, sizeof(*vq));
  size_t desc_sz, avail_sz, used_sz;
  if (!checked_ring_size(size, sizeof(struct vring_desc), 0, &desc_sz,
                         &vq->desc_pages) ||
      !checked_ring_size(size, sizeof(uint16_t), sizeof(struct vring_avail),
                         &avail_sz, &vq->avail_pages) ||
      !checked_ring_size(size, sizeof(struct vring_used_elem),
                         sizeof(struct vring_used), &used_sz, &vq->used_pages))
    return -EINVAL;
  vq->index = index;
  vq->size = size;
  vq->notify_off = notify_off;

  /* Allocate each ring on its own page(s) for alignment.
     bfc_alloc_page_data(n) returns a data pointer (not Page*), n = page count.
   */
  vq->desc = (struct vring_desc *)bfc_alloc_page_data(vq->desc_pages);
  vq->avail = (struct vring_avail *)bfc_alloc_page_data(vq->avail_pages);
  vq->used = (struct vring_used *)bfc_alloc_page_data(vq->used_pages);
  vq->next_free = (uint16_t *)kmalloc(size * sizeof(uint16_t));
  vq->ctx = (void **)kmalloc(size * sizeof(void *));
  vq->desc_state = (uint8_t *)kmalloc(size);

  if (!vq->desc || !vq->avail || !vq->used || !vq->next_free || !vq->ctx ||
      !vq->desc_state) {
    vring_destroy(vq);
    return -ENOMEM;
  }

  __memset(vq->desc, 0, desc_sz);
  __memset(vq->avail, 0, avail_sz);
  __memset(vq->used, 0, used_sz);
  __memset(vq->next_free, 0, size * sizeof(uint16_t));
  __memset(vq->ctx, 0, size * sizeof(void *));
  __memset(vq->desc_state, VRING_DESC_FREE, size);

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
  if (!vq)
    return;
  if (vq->desc)
    bfc_free_page_data(vq->desc, vq->desc_pages);
  if (vq->avail)
    bfc_free_page_data(vq->avail, vq->avail_pages);
  if (vq->used)
    bfc_free_page_data(vq->used, vq->used_pages);
  if (vq->next_free)
    kfree(vq->next_free);
  if (vq->ctx)
    kfree(vq->ctx);
  if (vq->desc_state)
    kfree(vq->desc_state);
  __memset(vq, 0, sizeof(*vq));
}

int vring_alloc_desc(struct virtqueue *vq) {
  if (!vq || !vq->size || vq->free_cnt == 0 || vq->free_head >= vq->size)
    return -1;
  int idx = vq->free_head;
  if (vq->desc_state[idx] != VRING_DESC_FREE) {
    vq->broken = true;
    return -1;
  }
  vq->free_head = vq->next_free[idx];
  vq->free_cnt--;
  vq->desc[idx].addr = 0;
  vq->desc[idx].len = 0;
  vq->desc[idx].flags = 0;
  vq->desc[idx].next = 0;
  vq->desc_state[idx] = VRING_DESC_BUILDING;
  return idx;
}

int vring_free_desc(struct virtqueue *vq, int idx) {
  if (!vq || idx < 0 || idx >= vq->size ||
      vq->desc_state[idx] == VRING_DESC_FREE || vq->free_cnt >= vq->size) {
    if (vq)
      vq->broken = true;
    return -EINVAL;
  }
  vq->desc_state[idx] = VRING_DESC_FREE;
  vq->ctx[idx] = NULL;
  vq->next_free[idx] = vq->free_head;
  vq->free_head = idx;
  vq->free_cnt++;
  return 0;
}

int vring_add_buf(struct virtqueue *vq, uint64_t *addrs, uint32_t *lens,
                  uint16_t *flags, int count, void *ctx) {
  if (!vq || !addrs || !lens || !flags || count <= 0 || count > vq->size ||
      vq->free_cnt < count || vq->broken)
    return -1;
  uint16_t allocated[VRING_MAX_SIZE];
  int allocated_count = 0;
  for (int i = 0; i < count; i++) {
    int idx = vring_alloc_desc(vq);
    if (idx < 0) {
      while (allocated_count)
        vring_free_desc(vq, allocated[--allocated_count]);
      return -1;
    }
    allocated[allocated_count++] = (uint16_t)idx;
    vq->desc[idx].addr = addrs[i];
    vq->desc[idx].len = lens[i];
    vq->desc[idx].flags =
        flags[i] & (VRING_DESC_F_WRITE | VRING_DESC_F_INDIRECT);
    if (i + 1 < count)
      vq->desc[idx].flags |= VRING_DESC_F_NEXT;
  }
  int head = allocated[0];
  for (int i = 0; i < count; i++) {
    uint16_t idx = allocated[i];
    if (i + 1 < count)
      vq->desc[idx].next = allocated[i + 1];
    vq->desc_state[idx] = VRING_DESC_AVAILABLE;
  }
  vq->ctx[head] = ctx;

  /* Publish to avail ring */
  vq->avail->ring[vq->avail_idx % vq->size] = head;
  vq->avail_idx++;

  return head;
}

void vring_kick(struct virtqueue *vq) {
  /* Ensure avail ring writes are visible before updating idx */
  __atomic_thread_fence(__ATOMIC_RELEASE);
  __atomic_store_n(&vq->avail->idx, vq->avail_idx, __ATOMIC_RELEASE);
  /* Actual notify (write to notify BAR) is done by caller via virtio_pci_notify
   */
}

bool vring_has_used(struct virtqueue *vq) {
  if (!vq || vq->broken)
    return false;
  uint16_t used = __atomic_load_n(&vq->used->idx, __ATOMIC_ACQUIRE);
  if ((uint16_t)(used - vq->used_idx) > vq->size) {
    vq->broken = true;
    return false;
  }
  return used != vq->used_idx;
}

int vring_poll_used(struct virtqueue *vq) {
  return vring_poll_used_budget(vq, vq ? vq->size : 0);
}

int vring_poll_used_budget(struct virtqueue *vq, uint16_t budget) {
  if (!vq || vq->broken || !budget)
    return 0;
  uint16_t device_used = __atomic_load_n(&vq->used->idx, __ATOMIC_ACQUIRE);
  uint16_t pending = (uint16_t)(device_used - vq->used_idx);
  if (pending > vq->size) {
    vq->broken = true;
    return -EPROTO;
  }
  int completions = 0;
  while (pending && completions < budget) {
    uint16_t idx = vq->used_idx % vq->size;
    struct vring_used_elem *e = &vq->used->ring[idx];
    uint32_t desc_id = e->id;
    if (desc_id >= vq->size ||
        vq->desc_state[desc_id] != VRING_DESC_AVAILABLE) {
      vq->broken = true;
      return -EPROTO;
    }
    void *callback_ctx = vq->ctx[desc_id];
    /* Walk the desc chain and free all descs */
    int cur = desc_id;
    uint16_t walked = 0;
    bool terminated = false;
    while (cur >= 0 && cur < vq->size && walked++ < vq->size) {
      if (vq->desc_state[cur] != VRING_DESC_AVAILABLE) {
        vq->broken = true;
        return -EPROTO;
      }
      uint16_t flags = vq->desc[cur].flags;
      int next = vq->desc[cur].next;
      if ((flags & VRING_DESC_F_NEXT) && (next < 0 || next >= vq->size)) {
        vq->broken = true;
        return -EPROTO;
      }
      if (vring_free_desc(vq, cur))
        return -EPROTO;
      if (!(flags & VRING_DESC_F_NEXT)) {
        terminated = true;
        break;
      }
      cur = next;
    }
    if (!terminated) {
      vq->broken = true;
      return -EPROTO;
    }
    /* Call callback on the head (ctx stored on head) */
    if (vq->callback)
      vq->callback(callback_ctx, e->len);
    vq->used_idx++;
    pending--;
    completions++;
  }
  return completions;
}

void virtio_ring_selftest(void) {
#ifdef TEST
  struct virtqueue vq;
  BUG_ON(vring_create(&vq, 0, 0, 0) != -EINVAL);
  BUG_ON(vring_create(&vq, 0, 3, 0) != -EINVAL);
  BUG_ON(vring_create(&vq, 0, 8, 0) != 0);
  uint64_t addrs[2] = {0x1000, 0x2000};
  uint32_t lens[2] = {16, 32};
  uint16_t flags[2] = {0, VRING_DESC_F_WRITE};
  int head = vring_add_buf(&vq, addrs, lens, flags, 2, NULL);
  BUG_ON(head < 0 || vq.free_cnt != 6);
  vring_kick(&vq);
  vq.used->ring[0].id = (uint32_t)head;
  vq.used->idx = 1;
  BUG_ON(vring_poll_used(&vq) != 1 || vq.free_cnt != 8 || vq.broken);
  head = vring_add_buf(&vq, addrs, lens, flags, 1, NULL);
  BUG_ON(head < 0);
  vring_kick(&vq);
  vq.used->ring[1].id = vq.size;
  vq.used->idx = 2;
  BUG_ON(vring_poll_used(&vq) != -EPROTO || !vq.broken);
  vring_destroy(&vq);
  printk(LOG_INFO, "virtio_ring: selftest passed\n");
#endif
}

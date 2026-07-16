/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#include "kernel/bsd/ring.h"

#include "arch/x64/smp.h"
#include "kernel/bsd/devtmpfs.h"
#include "kernel/bsd/proc.h"         /* struct proc: sig_pending/sig_blocked */
#include "kernel/bsd/types.h"        /* struct file */
#include "kernel/xcore/kpi.h"        /* copy_to_user */
#include "kernel/xcore/list.h"       /* list_init */
#include "kernel/xcore/mm_types.h"   /* struct shm */
#include "kernel/xcore/sched.h"      /* schedule, wake_with_event */
#include "kernel/xcore/sparse.h"     /* __force */
#include "kernel/xcore/wait_queue.h" /* add/remove_wait_queue, wait_queue_t */
#include "kernel/xcore/xtask.h"      /* current_task, BLOCKED, WAIT_POLL */

#include <xos/errno.h>
#include <xos/fcntl.h>
#include <xos/input.h>
#include <xos/signal.h>
#include <xos/socket.h> /* POLLIN */

ring_t ring_from_shm(struct shm *shm) {
  ring_t r = {NULL, NULL};
  if (!shm || !shm->phys)
    return r;
  struct ringbuf_header *hdr =
      (struct ringbuf_header *)phys_to_virt((__force phys_addr_t)shm->phys);
  if (!hdr || hdr->magic != RINGBUF_MAGIC)
    return r;
  r.hdr = hdr;
  r.data = (uint8_t *)hdr + hdr->data_offset;
  return r;
}

static void ring_wake_cb(wait_queue_t *wq, unsigned long flags) {
  xtask *target = (xtask *)wq->data;
  (void)flags;
  wake_wq_target(target);
}

ssize_t ring_read(ring_t *r, struct file *f, void *buf, size_t count) {
  if (!r->hdr)
    return -ENODEV;
  if (count == 0)
    return 0;
  struct ringbuf_header *hdr = r->hdr;
  uint32_t head = hdr->head;
  uint32_t cap = hdr->capacity;
  uint32_t esz = hdr->elem_size;
  uint32_t cursor = (uint32_t)f->offset;

  /* Slow reader: producer lapped us by a full turn or more — jump to head so
   * we read the newest events rather than stale overwritten slots. */
  uint32_t dist = (head >= cursor) ? (head - cursor) : (cap - cursor + head);
  if (dist >= cap)
    cursor = head;

  if (cursor == head) {
    if (f->flags & O_NONBLOCK)
      return -EAGAIN;
    /* 阻塞等待: 加入 per-file wq, prepare_to_wait 顺序（先挂 wq + 标 BLOCKED，
     * 再重查条件，最后 schedule）防丢唤醒。被 RINGBUF_WAKE 唤醒后重试。 */
    wait_queue_head *wq = file_wq_get(f);
    if (!wq)
      return -EAGAIN;
    wait_queue_t wait;
    wait.func = ring_wake_cb;
    wait.data = current_task;
    list_init(&wait.node);
    add_wait_queue(wq, &wait);

    while (1) {
      /* 先标 BLOCKED，再重查：写侧 __wake_up 若在重查后到达，task 已在 wq 且
       * BLOCKED，回调命中唤醒；若在重查前到达，重查发现就绪直接 break 不睡。 */
      current_task->state = BLOCKED;
      current_task->wait_event = WAIT_POLL;
      /* 重新检查条件 */
      if (!r->hdr || r->hdr->magic != RINGBUF_MAGIC) {
        sched_cancel_spurious_wake(current_task);
        remove_wait_queue(wq, &wait);
        return -ENODEV;
      }
      head = r->hdr->head;
      cursor = (uint32_t)f->offset;
      uint32_t dist2 =
          (head >= cursor) ? (head - cursor) : (cap - cursor + head);
      if (dist2 >= cap)
        cursor = head;
      if (cursor != head)
        break; /* 有数据 */
      if (f->flags & O_NONBLOCK) {
        sched_cancel_spurious_wake(current_task);
        remove_wait_queue(wq, &wait);
        return -EAGAIN;
      }
      /* signal_pending: 醒来须检查信号，否则吞 SIGKILL（与
       * socket.c:453/pty.c:88 同形态） */
      uint64_t pend =
          __atomic_load_n(&current_task->proc->sig_pending, __ATOMIC_ACQUIRE);
      uint64_t deliv = pend & ~current_task->proc->sig_blocked;
      deliv |= (pend & ((1ULL << SIGKILL) | (1ULL << SIGSTOP)));
      if (deliv) {
        sched_cancel_spurious_wake(current_task);
        remove_wait_queue(wq, &wait);
        return -EINTR;
      }
      schedule();
    }
    sched_cancel_spurious_wake(current_task);
    remove_wait_queue(wq, &wait);
  }

  uint32_t avail = (head > cursor) ? (head - cursor) : (cap - cursor + head);
  uint32_t n = avail;
  if (count / esz < n)
    n = count / esz;
  if (n == 0)
    return 0;

  for (uint32_t i = 0; i < n; i++) {
    uint32_t slot = (cursor + i) % cap;
    void *slot_addr = r->data + slot * esz;
    if (copy_to_user((char *)buf + i * esz, slot_addr, esz))
      return -EFAULT;
  }
  f->offset = (uint64_t)((cursor + n) % cap);
  return (ssize_t)(n * esz);
}

__poll ring_poll(ring_t *r, struct file *f, int events) {
  if (!r->hdr)
    return 0;
  uint32_t cursor = (uint32_t)f->offset;
  if (cursor != r->hdr->head)
    return events & POLLIN;
  return 0;
}

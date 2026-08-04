/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/driver/virtio_gpu.h"

#include <stdbool.h>
#include <stddef.h>

#include "arch/x64/apic.h"
#include "arch/x64/paging.h"
#include "arch/x64/smp.h"
#include "arch/x64/trap.h"
#include "arch/x64/utils.h"
#include "kernel/bsd/devtmpfs.h"
#include "kernel/bsd/kfcntl.h" // IWYU pragma: keep
#include "kernel/bsd/poll_types.h"
#include "kernel/bsd/sysfs.h"
#include "kernel/driver/bsd_types.h"
#include "kernel/driver/driver.h"
#include "kernel/driver/drm_internal.h"
#include "kernel/driver/pci.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/mem/kasan.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/mem/vma.h"
#include "kernel/xcore/mm_types.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/trap.h"
#include "kernel/xcore/wait_queue.h"
#include "kernel/xcore/xtask.h"
#include "utils/macro.h"

#include <xos/errno.h>
#include <xos/page.h>
#include <xos/socket.h>

#include "drm/drm.h"
#include "drm/drm_fourcc.h"
#include "drm/drm_mode.h"
#include "drm/virtgpu_drm.h"

#define DRM_MAJOR 226

struct virtio_gpu_device g_virtio_gpu;
struct drm_device g_drm;
static uint32_t drm_page_flip_log_count;
static uint32_t drm_flip_event_log_count;
struct drm_property g_drm_properties[DRM_MAX_PROPERTIES];
int g_drm_next_prop_id = 1;
struct drm_blob g_drm_blobs[DRM_MAX_BLOBS];
int g_drm_next_blob_id = 1;
spinlock g_drm_files_lock = SPINLOCK_INIT;
struct drm_file g_drm_files[MAX_DRM_FDS];
struct drm_cursor g_drm_cursor;

/* Single vring completion callback shared by sync and async paths. The ctx is
 * either a struct virtgpu_sync_ctx (sync send_cmd path) or a
 * struct virtgpu_cmd_pending (async EXECBUFFER path); the tag discriminates.
 * Runs in ISR (under cmd_lock via vring_poll_used). Both ctx structs are
 * defined in virtio_gpu.h. */
static void virtio_gpu_cmd_callback(void *ctx, uint32_t len) {
  /* Defensive: ctx must never be NULL in normal operation (it is the
     per-cmd context set in vring_add_buf).  A NULL here means the used
     ring was drained against a descriptor whose ctx had already been
     cleared/reused by a concurrent drain — historically the direct cause
     of the NULL-write #PF.  Harmless-ize it rather than crashing. */
  if (!ctx)
    return;
  enum virtgpu_cmd_ctx_tag tag = *(enum virtgpu_cmd_ctx_tag *)ctx;
  if (tag == VIRTGPU_CTX_ASYNC) {
    struct virtgpu_cmd_pending *pn = (struct virtgpu_cmd_pending *)ctx;
    pn->response_ready = true;
  } else {
    struct virtgpu_sync_ctx *sc = (struct virtgpu_sync_ctx *)ctx;
    sc->completed = true;
  }
  (void)len;
}

/* Wake callback for wait_queue: bridges __wake_up → wake_wq_target.
   队列身份制：cmd_wq 与资源 wq 物理隔离，跨源不可达；task 在 cmd_wq 上即唤醒，
   不查 wait_event。锁序：cmd_wq.lock → scheduler_lock（A-class wq→sched）。 */
static void virtio_gpu_wake_cb(wait_queue_t *wq, unsigned long flags) {
  xtask *target = (xtask *)wq->data;
  (void)flags;
  wake_wq_target(target);
}

/* Forward declarations */
static void virtio_gpu_isr(trapframe *tf);
static int virtio_gpu_send_cmd(struct virtio_gpu_device *vgpu, void *cmd_buf,
                               size_t cmd_len, void *resp_buf, size_t resp_len);
extern void drm_dev_register(void);
extern dev_driver virtio_gpu_driver;

/* BSD-layer KPI for sync_file fd install/lookup (plan2). Declared here as
 * extern — not via kernel/bsd/proc.h — so the driver stays off the bsd include
 * boundary (only devtmpfs/sysfs/poll_types are allowed). drm_fence is opaque to
 * the BSD layer; the pointer is just handed across. */
int bsd_sync_file_fd_install(xtask *proc, struct drm_fence *fence);
int bsd_drm_prime_fd_install(xtask *proc, struct drm_prime_object *object,
                             bool cloexec);
struct file *bsd_drm_prime_fd_get(xtask *proc, int fd);

/* plan2 forward declarations: these are defined later in the file but used by
 * the ISR (drm_fence_find/signal) and the EXECBUFFER ioctl (drm_file_current)
 * which precede their definitions. */
static struct drm_fence *drm_fence_find(uint32_t ctx_id, uint8_t ring_idx,
                                        uint64_t fence_id);
static void drm_fence_signal(struct drm_fence *fence);
static long drm_ioctl_prime_handle_to_fd(void *arg, xtask *proc);
static long drm_ioctl_prime_fd_to_handle(void *arg, xtask *proc,
                                         struct drm_file *df);

/* ===== 2.B: ctrlq initialization ===== */

/* Initialize ctrlq: query queue size, allocate rings, set up common cfg */
static int virtio_gpu_init_ctrlq(struct virtio_gpu_device *vgpu) {
  struct virtio_pci_dev *vpci = &vgpu->vpci;
  struct virtio_pci_common_cfg __iomem *common = vpci->common;

  /* Select queue 0 (ctrlq) */
  common->queue_select = VIRTIO_GPU_CTRLQ_INDEX;
  uint16_t size = common->queue_size;
  uint16_t notify_off = common->queue_notify_off;
  if (size == 0 || size > 1024) {
    printk(LOG_ERROR, "virtio_gpu: invalid ctrlq size %u\n", size);
    return -1;
  }
  printk(LOG_INFO, "virtio_gpu: ctrlq size=%u notify_off=%u\n", size,
         notify_off);

  /* Allocate and initialize the virtqueue */
  if (vring_create(&vgpu->ctrlq, VIRTIO_GPU_CTRLQ_INDEX, size, notify_off) <
      0) {
    printk(LOG_ERROR, "virtio_gpu: vring_create failed\n");
    return -1;
  }

  /* Program queue addresses into common config */
  common->queue_desc_lo = (uint32_t)(vgpu->ctrlq.desc_phys & 0xFFFFFFFF);
  common->queue_desc_hi = (uint32_t)(vgpu->ctrlq.desc_phys >> 32);
  common->queue_avail_lo = (uint32_t)(vgpu->ctrlq.avail_phys & 0xFFFFFFFF);
  common->queue_avail_hi = (uint32_t)(vgpu->ctrlq.avail_phys >> 32);
  common->queue_used_lo = (uint32_t)(vgpu->ctrlq.used_phys & 0xFFFFFFFF);
  common->queue_used_hi = (uint32_t)(vgpu->ctrlq.used_phys >> 32);

  /* Assign MSI-X vector to this queue (set before enable).
     Per virtio spec 1.1 §4.1.4.3, queue_msix_vector is the MSI-X **table entry
     index** (0-based), NOT the LAPIC vector number.  The device maps entry
     index → LAPIC vector via its internal MSI-X table.  Writing the LAPIC
     vector (69) causes the device to reject it (0xFFFF) since only entries 0..1
     exist. */
  common->queue_msix_vector = 0; /* MSI-X table entry 0 (queue interrupt) */
  uint16_t accepted_vec = common->queue_msix_vector;
  printk(LOG_INFO,
         "virtio_gpu: queue_msix_vector entry=%u readback=%u (lapic_vec=%u)\n",
         0, accepted_vec, vgpu->vpci.msix_vector);

  /* Enable queue */
  common->queue_enable = 1;

  return 0;
}

/* ===== 2.C: ISR + sleep/wake command synchronization ===== */

/* ISR: called when virtio-gpu raises MSI-X interrupt.
   Reads ISR capability to distinguish queue interrupt vs config change,
   drains used ring, wakes any waiting task. */
static void virtio_gpu_isr(trapframe *tf) {
  struct virtio_gpu_device *vgpu = &g_virtio_gpu;
  uint8_t isr_status = virtio_pci_read_isr(&vgpu->vpci);

  if (isr_status & VIRTIO_ISR_QUEUE_INTR) {
    /* Drain the used ring under cmd_lock so vring_poll_used (frees descs,
       clears ctx[], advances used_idx) is mutually exclusive with the
       process side's vring_add_buf (allocates descs, sets ctx[], publishes
       avail).  This is the single place the used ring is drained now.
       irqsave is symmetric with send_cmd's process-side acquisition: while
       cmd_lock is held here, the originating CPU cannot re-enter this ISR.
       vring_poll_used invokes virtio_gpu_cmd_callback for each used desc,
       which sets cmd_ctx.completed (sync path) or pending->response_ready
       (async path). */
    uint64_t flags;
    spin_lock_irqsave(&vgpu->cmd_lock, &flags);
    vring_poll_used(&vgpu->ctrlq);
    spin_unlock_irqrestore(&vgpu->cmd_lock, flags);

    /* Walk pending_list: for each completed async cmd, signal its fence, wake
     * any waiter, unlink and free (cmd_buf/resp_buf owned by the node). Lock
     * order is non-nested with cmd_lock above (both fully released between),
     * matching the async submit path. */
    spin_lock_irqsave(&vgpu->pending_lock, &flags);
    struct virtgpu_cmd_pending *p = vgpu->pending_list;
    struct virtgpu_cmd_pending *prev = NULL;
    while (p) {
      if (p->response_ready) {
        if (p->hdr.flags & VIRTIO_GPU_FLAG_FENCE) {
          struct drm_fence *f =
              drm_fence_find(p->hdr.ctx_id, p->hdr.ring_idx, p->hdr.fence_id);
          drm_fence_signal(f);
          drm_fence_put(f); /* drop the in-flight submission reference */
        }
        if (p->waiter)
          wake_wq_target(p->waiter);
        struct virtgpu_cmd_pending *done = p;
        p = p->next;
        if (prev)
          prev->next = p;
        else
          vgpu->pending_list = p;
        kfree(done->cmd_buf);
        kfree(done->resp_buf);
        kfree(done);
      } else {
        prev = p;
        p = p->next;
      }
    }
    spin_unlock_irqrestore(&vgpu->pending_lock, flags);

    __wake_up(&vgpu->cmd_wq, 0); /* sync-path sleepers */
  }
  /* config change: not handled (no EDID) */

  lapic_eoi();
}

/* Send a command and wait for response (synchronous).
   cmd_buf: pointer to command struct (e.g. virtio_gpu_resource_create_2d)
   cmd_len: command size in bytes
   resp_buf: pointer to response buffer (caller-allocated)
   resp_len: response buffer size
   Returns 0 on success (response received), negative on error. */
static int virtio_gpu_send_cmd(struct virtio_gpu_device *vgpu, void *cmd_buf,
                               size_t cmd_len, void *resp_buf,
                               size_t resp_len) {
  /* Per-command completion context: vring callback sets completed=true
     when the device processes this descriptor.  Each caller has its own
     ctx on the stack, so concurrent send_cmd invocations don't clobber
     each other's state. */
  struct virtgpu_sync_ctx cmd_ctx = {.tag = VIRTGPU_CTX_SYNC,
                                     .completed = false};

  /* Physical addresses for descriptors (must be guest-physical) */
  uint64_t cmd_phys = (uint64_t)PHY_ADDR((uintptr_t)cmd_buf);
  uint64_t resp_phys = (uint64_t)PHY_ADDR((uintptr_t)resp_buf);

  /* Set up 2 descriptors: cmd (device-readable) + resp (device-writable) */
  uint64_t addrs[2] = {cmd_phys, resp_phys};
  uint32_t lens[2] = {(uint32_t)cmd_len, (uint32_t)resp_len};
  uint16_t flags[2] = {0, VRING_DESC_F_WRITE}; /* cmd: read-only; resp: write */

  /* During early boot (driver_init, before idle process exists) there is no
     process context to sleep in: current_task is NULL and schedule() cannot
     block. Poll the used ring synchronously instead. */
  if (current_task == NULL) {
    spin_lock(&vgpu->cmd_lock);
    int head = vring_add_buf(&vgpu->ctrlq, addrs, lens, flags, 2, &cmd_ctx);
    if (head < 0) {
      spin_unlock(&vgpu->cmd_lock);
      printk(LOG_ERROR, "virtio_gpu: vring_add_buf failed\n");
      return -1;
    }
    vring_kick(&vgpu->ctrlq);
    virtio_pci_notify(&vgpu->vpci, vgpu->ctrlq.notify_off);
    while (!vring_has_used(&vgpu->ctrlq)) {
      __asm__ volatile("pause" ::: "memory");
    }
    vring_poll_used(&vgpu->ctrlq); /* callback sets cmd_ctx.completed */
    spin_unlock(&vgpu->cmd_lock);
    return cmd_ctx.completed ? 0 : -1;
  }

  /* Process context: register on wait queue, submit, and sleep in a loop.
     The loop handles spurious wakes (__wake_up wakes all waiters; those
     whose command hasn't completed yet re-sleep).  The "set BLOCKED →
     re-check → schedule" pattern prevents lost wakeup: if the ISR fires
     between setting BLOCKED and calling schedule(), wake_with_event sets
     state to READY, and schedule() returns immediately. */
  wait_queue_t wait;
  wait.func = virtio_gpu_wake_cb;
  wait.data = current_task;
  wait.exclusive = 0;
  list_init(&wait.node);
  add_wait_queue(&vgpu->cmd_wq, &wait);

  /* Hold cmd_lock with interrupts disabled: the virtio-gpu ISR also takes
     cmd_lock to drain the used ring, so acquiring it irqsave on the
     process side prevents a same-CPU ISR re-entry from deadlocking, and
     makes the alloc/publish side of vring_add_buf mutually exclusive with
     the ISR's drain side.  Use irq_flags to avoid clashing with the
     descriptor flags[] array below. */
  uint64_t irq_flags;
  spin_lock_irqsave(&vgpu->cmd_lock, &irq_flags);
  int head = vring_add_buf(&vgpu->ctrlq, addrs, lens, flags, 2, &cmd_ctx);
  if (head < 0) {
    spin_unlock_irqrestore(&vgpu->cmd_lock, irq_flags);
    remove_wait_queue(&vgpu->cmd_wq, &wait);
    printk(LOG_ERROR, "virtio_gpu: vring_add_buf failed\n");
    return -1;
  }

  /* Arm BLOCKED state before kick — lost-wakeup-safe: after we release
     cmd_lock (re-enabling interrupts) the ISR may fire immediately, see
     BLOCKED, and wake us via __wake_up(&cmd_wq). 队列身份制：cmd_wq 与资源 wq
     物理隔离，能唤醒 GPU 等待者的只有 ISR 对 cmd_wq 的 __wake_up，跨源不可达；
     此处 arm 防 unlock(cmd_lock) → schedule() 之间 ISR 在首轮 schedule 前
     fire。 */
  current_task->state = BLOCKED;

  vring_kick(&vgpu->ctrlq);
  virtio_pci_notify(&vgpu->vpci, vgpu->ctrlq.notify_off);

  /* Release lock before sleeping — the ISR drains the used ring under
     cmd_lock.  If the ISR already completed our command it set
     cmd_ctx.completed=true and (via __wake_up) enqueued our run_node;
     schedule() below dequeues it and runs us. */
  spin_unlock_irqrestore(&vgpu->cmd_lock, irq_flags);

  /* Wait loop.  We go through schedule() on EVERY iteration (do/while, not a
     pre-test that early-exits): schedule() is the only place our run_node is
     dequeued from the run_queue.  If the ISR already enqueued run_node (fast
     completion), schedule() dequeues it and re-runs us; if not, we block
     until the ISR wakes us via __wake_up(&cmd_wq). 队列身份制：被唤醒 ⇔ cmd_wq
     wake ⇔ ISR 完成 ⇔ cmd_ctx.completed，循环只在真完成时退出；cmd_wq 与资源 wq
     物理隔离，跨源不可达。used ring 仅由 ISR drain，此处不轮询。 */
  do {
    current_task->state = BLOCKED;
    schedule();
  } while (!cmd_ctx.completed);

  remove_wait_queue(&vgpu->cmd_wq, &wait);
  return cmd_ctx.completed ? 0 : -1;
}

/* 3D/context/blob commands: identical to send_cmd (2-descriptor cmd+resp,
 * synchronous). Kept as a separate entry point so plan2 can swap in an async
 * variant without touching the 2D path. */
int virtio_gpu_send_cmd_3d(struct virtio_gpu_device *vgpu, void *cmd_buf,
                           size_t cmd_len, void *resp_buf, size_t resp_len) {
  return virtio_gpu_send_cmd(vgpu, cmd_buf, cmd_len, resp_buf, resp_len);
}

/* Async 3D submit: kmalloc private cmd+resp copies owned by a pending node,
 * enqueue on pending_list, add_buf+kick under cmd_lock (NOT held across any
 * sleep), return immediately. ISR completes via pending_list walk and frees
 * the node + buffers. Multiple commands may be in-flight concurrently
 * (bounded by vring desc count). Returns 0 on submitted, negative on error.
 *
 * caller_cmd/caller_resp are only used as copy sources; their lifetime after
 * return is irrelevant. fence_id/ring_idx/ctx_id are recorded into the node's
 * hdr copy so the ISR can find the matching fence. */
static int virtio_gpu_send_cmd_3d_async(struct virtio_gpu_device *vgpu,
                                        const void *caller_cmd, size_t cmd_len,
                                        const void *caller_resp,
                                        size_t resp_len, uint64_t fence_id,
                                        uint8_t ring_idx, uint32_t ctx_id) {
  struct virtgpu_cmd_pending *pn = kmalloc(sizeof(*pn));
  if (!pn)
    return -ENOMEM;
  pn->tag = VIRTGPU_CTX_ASYNC;
  pn->response_ready = false;
  pn->waiter = NULL;
  pn->next = NULL;
  pn->cmd_len = (uint32_t)cmd_len;
  pn->resp_len = (uint32_t)resp_len;
  pn->cmd_buf = kmalloc(cmd_len);
  pn->resp_buf = kmalloc(resp_len);
  if (!pn->cmd_buf || !pn->resp_buf) {
    kfree(pn->cmd_buf);
    kfree(pn->resp_buf);
    kfree(pn);
    return -ENOMEM;
  }
  __memcpy(pn->cmd_buf, caller_cmd, cmd_len);
  __memset(pn->resp_buf, 0, resp_len);
  /* Copy the ctrl_hdr (first sizeof(ctrl_hdr) bytes of cmd) for ISR fence
   * lookup. */
  __memcpy(&pn->hdr, pn->cmd_buf, sizeof(struct virtio_gpu_ctrl_hdr));
  /* Ensure the fence fields in the node hdr match what we tell the host. */
  pn->hdr.fence_id = fence_id;
  pn->hdr.ring_idx = ring_idx;
  pn->hdr.ctx_id = ctx_id;

  /* Link onto pending_list (under pending_lock) before submit so the ISR can
   * always find the node. */
  uint64_t pflags;
  spin_lock_irqsave(&vgpu->pending_lock, &pflags);
  pn->next = vgpu->pending_list;
  vgpu->pending_list = pn;
  spin_unlock_irqrestore(&vgpu->pending_lock, pflags);

  /* Submit: cmd_lock ONLY covers add_buf+kick (free-list serialization), NOT
   * any sleep. This is what lets the next EXECBUFFER submit while this one is
   * still on the host. */
  uint64_t cmd_phys = (uint64_t)PHY_ADDR((uintptr_t)pn->cmd_buf);
  uint64_t resp_phys = (uint64_t)PHY_ADDR((uintptr_t)pn->resp_buf);
  uint64_t addrs[2] = {cmd_phys, resp_phys};
  uint32_t lens[2] = {(uint32_t)cmd_len, (uint32_t)resp_len};
  uint16_t dflags[2] = {0, VRING_DESC_F_WRITE};

  uint64_t cflags;
  spin_lock_irqsave(&vgpu->cmd_lock, &cflags);
  int head = vring_add_buf(&vgpu->ctrlq, addrs, lens, dflags, 2, pn);
  if (head < 0) {
    spin_unlock_irqrestore(&vgpu->cmd_lock, cflags);
    /* unlink pending node and free */
    spin_lock_irqsave(&vgpu->pending_lock, &pflags);
    vgpu->pending_list = pn->next;
    spin_unlock_irqrestore(&vgpu->pending_lock, pflags);
    kfree(pn->cmd_buf);
    kfree(pn->resp_buf);
    kfree(pn);
    return -ENOMEM;
  }
  vring_kick(&vgpu->ctrlq);
  virtio_pci_notify(&vgpu->vpci, vgpu->ctrlq.notify_off);
  spin_unlock_irqrestore(&vgpu->cmd_lock, cflags);
  return 0;
}

/* ===== 2.D: high-level command wrappers ===== */

int virtio_gpu_create_2d(uint32_t resource_id, uint32_t width, uint32_t height,
                         uint32_t format) {
  struct virtio_gpu_resource_create_2d cmd;
  __memset(&cmd, 0, sizeof(cmd));
  cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
  cmd.resource_id = resource_id;
  cmd.format = format;
  cmd.width = width;
  cmd.height = height;

  struct virtio_gpu_ctrl_hdr_response resp;
  __memset(&resp, 0, sizeof(resp));

  int rc = virtio_gpu_send_cmd(&g_virtio_gpu, &cmd, sizeof(cmd), &resp,
                               sizeof(resp));
  if (rc < 0)
    return rc;
  if (resp.hdr.type != VIRTIO_GPU_RESP_OK_NODATA) {
    printk(LOG_ERROR, "virtio_gpu: CREATE_2D failed, resp type=0x%x\n",
           resp.hdr.type);
    return -1;
  }
  return 0;
}

int virtio_gpu_attach_backing(uint32_t resource_id, uint64_t guest_phys,
                              uint32_t length) {
  /* Command + 1 mem entry in a single buffer */
  struct {
    struct virtio_gpu_resource_attach_backing cmd;
    struct virtio_gpu_mem_entry entry;
  } __attribute__((packed)) buf;
  __memset(&buf, 0, sizeof(buf));
  buf.cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
  buf.cmd.resource_id = resource_id;
  buf.cmd.nr_entries = 1;
  buf.entry.addr = guest_phys;
  buf.entry.length = length;

  struct virtio_gpu_ctrl_hdr_response resp;
  __memset(&resp, 0, sizeof(resp));

  int rc = virtio_gpu_send_cmd(&g_virtio_gpu, &buf, sizeof(buf), &resp,
                               sizeof(resp));
  if (rc < 0)
    return rc;
  if (resp.hdr.type != VIRTIO_GPU_RESP_OK_NODATA) {
    printk(LOG_ERROR, "virtio_gpu: ATTACH_BACKING failed, resp type=0x%x\n",
           resp.hdr.type);
    return -1;
  }
  return 0;
}

int virtio_gpu_set_scanout(uint32_t scanout_id, uint32_t resource_id,
                           uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  struct virtio_gpu_set_scanout cmd;
  __memset(&cmd, 0, sizeof(cmd));
  cmd.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
  cmd.r.x = x;
  cmd.r.y = y;
  cmd.r.width = w;
  cmd.r.height = h;
  cmd.scanout_id = scanout_id;
  cmd.resource_id = resource_id;

  struct virtio_gpu_ctrl_hdr_response resp;
  __memset(&resp, 0, sizeof(resp));

  int rc = virtio_gpu_send_cmd(&g_virtio_gpu, &cmd, sizeof(cmd), &resp,
                               sizeof(resp));
  if (rc < 0)
    return rc;
  if (resp.hdr.type != VIRTIO_GPU_RESP_OK_NODATA) {
    printk(LOG_ERROR, "virtio_gpu: SET_SCANOUT failed, resp type=0x%x\n",
           resp.hdr.type);
    return -1;
  }
  return 0;
}

int virtio_gpu_transfer_2d(uint32_t resource_id, uint32_t x, uint32_t y,
                           uint32_t w, uint32_t h, uint64_t offset) {
  struct virtio_gpu_transfer_to_host_2d cmd;
  __memset(&cmd, 0, sizeof(cmd));
  cmd.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
  cmd.r.x = x;
  cmd.r.y = y;
  cmd.r.width = w;
  cmd.r.height = h;
  cmd.offset = offset;
  cmd.resource_id = resource_id;

  struct virtio_gpu_ctrl_hdr_response resp;
  __memset(&resp, 0, sizeof(resp));

  int rc = virtio_gpu_send_cmd(&g_virtio_gpu, &cmd, sizeof(cmd), &resp,
                               sizeof(resp));
  if (rc < 0)
    return rc;
  if (resp.hdr.type != VIRTIO_GPU_RESP_OK_NODATA) {
    printk(LOG_ERROR,
           "virtio_gpu: TRANSFER_TO_HOST_2D failed, resp type=0x%x\n",
           resp.hdr.type);
    return -1;
  }
  return 0;
}

int virtio_gpu_flush(uint32_t resource_id, uint32_t x, uint32_t y, uint32_t w,
                     uint32_t h) {
  struct virtio_gpu_resource_flush cmd;
  __memset(&cmd, 0, sizeof(cmd));
  cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
  cmd.r.x = x;
  cmd.r.y = y;
  cmd.r.width = w;
  cmd.r.height = h;
  cmd.resource_id = resource_id;

  struct virtio_gpu_ctrl_hdr_response resp;
  __memset(&resp, 0, sizeof(resp));

  int rc = virtio_gpu_send_cmd(&g_virtio_gpu, &cmd, sizeof(cmd), &resp,
                               sizeof(resp));
  if (rc < 0)
    return rc;
  if (resp.hdr.type != VIRTIO_GPU_RESP_OK_NODATA) {
    printk(LOG_ERROR, "virtio_gpu: RESOURCE_FLUSH failed, resp type=0x%x\n",
           resp.hdr.type);
    return -1;
  }
  return 0;
}

int virtio_gpu_resource_unref(uint32_t resource_id) {
  struct virtio_gpu_resource_unref cmd;
  __memset(&cmd, 0, sizeof(cmd));
  cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
  cmd.resource_id = resource_id;

  struct virtio_gpu_ctrl_hdr_response resp;
  __memset(&resp, 0, sizeof(resp));

  int rc = virtio_gpu_send_cmd(&g_virtio_gpu, &cmd, sizeof(cmd), &resp,
                               sizeof(resp));
  if (rc < 0)
    return rc;
  if (resp.hdr.type != VIRTIO_GPU_RESP_OK_NODATA) {
    printk(LOG_ERROR, "virtio_gpu: RESOURCE_UNREF failed, resp type=0x%x\n",
           resp.hdr.type);
    return -1;
  }
  return 0;
}

/* ===== 2.E: real init + driver definition ===== */

/* ===== DRM ioctl implementation ===== */

static struct drm_dumb_buffer *drm_find_dumb(int handle) {
  if (handle <= 0 || handle > MAX_DUMB_BUFFERS)
    return NULL;
  struct drm_dumb_buffer *d = &g_drm.dumbs[handle - 1];
  return (d->handle == handle) ? d : NULL;
}

static struct drm_framebuffer *drm_find_fb(int fb_id) {
  if (fb_id <= 0 || fb_id > MAX_FRAMEBUFFERS)
    return NULL;
  struct drm_framebuffer *fb = &g_drm.fbs[fb_id - 1];
  return (fb->fb_id == fb_id) ? fb : NULL;
}

static int drm_alloc_dumb_handle(void) {
  for (int i = 0; i < MAX_DUMB_BUFFERS; i++) {
    if (g_drm.dumbs[i].handle == 0) {
      g_drm.dumbs[i].handle = i + 1; /* handle = slot index + 1 */
      g_drm.dumbs[i].refcount = 1;
      return g_drm.dumbs[i].handle;
    }
  }
  return -1;
}

/* ===== Property infrastructure helpers (Phase C) ===== */
/* File-level obj_props_tbl — shared between add_to_object and get,
   avoiding the bug of duplicate per-function static arrays. */
static struct drm_object_props obj_props_tbl[8];
static bool obj_props_inited = false;

static struct drm_property *drm_find_property(uint32_t prop_id) {
  if (prop_id == 0 || prop_id > DRM_MAX_PROPERTIES)
    return NULL;
  struct drm_property *p = &g_drm_properties[prop_id - 1];
  if (!p->allocated)
    return NULL;
  return p;
}

static struct drm_blob *drm_find_blob(uint32_t blob_id) {
  if (blob_id == 0 || blob_id > DRM_MAX_BLOBS)
    return NULL;
  struct drm_blob *b = &g_drm_blobs[blob_id - 1];
  if (!b->allocated)
    return NULL;
  return b;
}

/* ===== Fence (plan2) ===== */

/* Find or allocate a fence slot. Returns fence with refcount=1, or NULL if
 * table full. Caller (process context) owns the initial ref. */
static struct drm_fence *drm_fence_create(uint32_t ctx_id, uint8_t ring_idx,
                                          uint64_t fence_id) {
  uint64_t flags;
  spin_lock_irqsave(&g_drm.fence_lock, &flags);
  for (int i = 0; i < MAX_FENCES; i++) {
    if (!g_drm.fences[i].ctx_id) {
      struct drm_fence *f = &g_drm.fences[i];
      f->ctx_id = ctx_id;
      f->ring_idx = ring_idx;
      f->fence_id = fence_id;
      f->signaled = false;
      refcount_set(&f->refcount, 1);
      f->lock = SPINLOCK_INIT;
      init_wait_queue_head(&f->wq);
      spin_unlock_irqrestore(&g_drm.fence_lock, flags);
      return f;
    }
  }
  spin_unlock_irqrestore(&g_drm.fence_lock, flags);
  return NULL;
}

static struct drm_fence *drm_fence_find(uint32_t ctx_id, uint8_t ring_idx,
                                        uint64_t fence_id) {
  uint64_t flags;
  spin_lock_irqsave(&g_drm.fence_lock, &flags);
  for (int i = 0; i < MAX_FENCES; i++) {
    if (g_drm.fences[i].ctx_id == ctx_id &&
        g_drm.fences[i].ring_idx == ring_idx &&
        g_drm.fences[i].fence_id == fence_id) {
      spin_unlock_irqrestore(&g_drm.fence_lock, flags);
      return &g_drm.fences[i];
    }
  }
  spin_unlock_irqrestore(&g_drm.fence_lock, flags);
  return NULL;
}

/* Drop a ref; when refcount hits 0, reclaim the slot. This is also called by
 * the virtio-gpu ISR when an in-flight submission completes, so fence_lock
 * must always be acquired with IRQs disabled. */
void drm_fence_put(struct drm_fence *fence) {
  if (!fence)
    return;
  /* Reclaim under fence_lock so the table-scanned free check is atomic. */
  uint64_t flags;
  spin_lock_irqsave(&g_drm.fence_lock, &flags);
  if (!refcount_dec_and_test(&fence->refcount)) {
    spin_unlock_irqrestore(&g_drm.fence_lock, flags);
    return;
  }
  fence->ctx_id = 0; /* mark slot free */
  spin_unlock_irqrestore(&g_drm.fence_lock, flags);
}

/* Read-only signaled probe for sync_file poll (BSD file_poll.c). */
bool drm_fence_is_signaled(struct drm_fence *fence) {
  if (!fence)
    return false;
  uint64_t flags;
  spin_lock_irqsave(&fence->lock, &flags);
  bool s = fence->signaled;
  spin_unlock_irqrestore(&fence->lock, flags);
  return s;
}

/* ISR: mark fence signaled and wake waiters. Runs in interrupt context — MUST
 * use irqsave. Does NOT free the fence (refcount may be held by a sync_file
 * fd); only marks signaled. */
static void drm_fence_signal(struct drm_fence *fence) {
  if (!fence)
    return;
  uint64_t flags;
  spin_lock_irqsave(&fence->lock, &flags);
  if (fence->signaled) {
    spin_unlock_irqrestore(&fence->lock, flags);
    return;
  }
  fence->signaled = true;
  spin_unlock_irqrestore(&fence->lock, flags);

  spin_lock_irqsave(&g_drm.fence_lock, &flags);
  if (fence->ctx_id > 0 && fence->ctx_id <= MAX_CTX_IDS &&
      fence->ring_idx < MAX_CTX_RINGS) {
    uint64_t *completed =
        &g_drm.completed_fence_ids[fence->ctx_id - 1][fence->ring_idx];
    if (*completed < fence->fence_id)
      *completed = fence->fence_id;
  }
  spin_unlock_irqrestore(&g_drm.fence_lock, flags);
  __wake_up(&fence->wq, 0);
}

/* Block on fence->wq until signaled or timeout_ns elapses. timeout_ns==0 →
 * wait forever. Returns 0 on signal, -ETIME on timeout. Currently unused by
 * the EXECBUFFER path (which polls the sync_file fd); retained as the fence
 * wait primitive for future in-kernel waits (plan2 2A-3 / acceptance #7). */
static __attribute__((unused)) int drm_fence_wait(struct drm_fence *fence,
                                                  uint64_t timeout_ns) {
  if (!fence)
    return -EINVAL;
  wait_queue_t wait;
  wait.func = virtio_gpu_wake_cb; /* reuse: data=current_task, wake_wq_target */
  wait.data = current_task;
  wait.exclusive = 0;
  list_init(&wait.node);
  add_wait_queue(&fence->wq, &wait);

  uint64_t deadline = (timeout_ns != 0) ? sched_clock() + timeout_ns : 0;
  int ret = 0;
  for (;;) {
    current_task->state = BLOCKED;
    if (fence->signaled)
      break;
    if (timeout_ns != 0 && sched_clock() >= deadline) {
      ret = -ETIME;
      break;
    }
    schedule();
  }
  // prepare_to_wait: the loop marks BLOCKED at the top. If a concurrent
  // fence ISR woke us (state=READY + run_node pushed) between marking BLOCKED
  // and the signaled/reached check, breaking out without schedule() leaves a
  // dangling run_node — a steal would later ASSERT(state==READY) on a RUNNING
  // task. Cancel any such spurious wake; RUNNING is unconditional because the
  // first-iteration break path never ran schedule() (state still BLOCKED).
  current_task->state = RUNNING;
  sched_cancel_spurious_wake(current_task);
  remove_wait_queue(&fence->wq, &wait);
  return ret;
}

/* Install a sync_file fd bound to a fence. Takes a ref on the fence (released
 * when the fd is closed via file_put's switch case for FD_SYNC_FILE). poll(fd)
 * returns POLLIN once fence->signaled. Modeled on eventfd/timerfd fd install.
 *
 * fd-table install is delegated to the BSD KPI bsd_sync_file_fd_install so the
 * driver never touches struct file / fd table layout directly (driver↔bsd
 * include boundary). We still own the fence refcount: take a ref here, hand it
 * to the fd, drop it back if install fails. */
static int drm_fence_install_sync_file(struct drm_fence *fence, xtask *proc) {
  if (!fence)
    return -EINVAL;
  /* Take a ref for the fd while excluding the completion ISR. */
  uint64_t flags;
  spin_lock_irqsave(&g_drm.fence_lock, &flags);
  refcount_inc(&fence->refcount);
  spin_unlock_irqrestore(&g_drm.fence_lock, flags);

  int fd = bsd_sync_file_fd_install(proc, fence);
  if (fd < 0)
    drm_fence_put(fence); /* reclaim the ref the fd won't be holding */
  return fd;
}

static uint32_t drm_property_create_range(const char *name, uint32_t min,
                                          uint32_t max, bool is_immutable) {
  int id = g_drm_next_prop_id++;
  if (id > DRM_MAX_PROPERTIES) {
    g_drm_next_prop_id = DRM_MAX_PROPERTIES + 1;
    return 0;
  }
  struct drm_property *p = &g_drm_properties[id - 1];
  p->prop_id = (uint32_t)id;
  p->allocated = true;
  __memset(p->name, 0, sizeof(p->name));
  size_t nlen = 0;
  while (name[nlen] && nlen < DRM_PROP_NAME_LEN - 1) {
    p->name[nlen] = name[nlen];
    nlen++;
  }
  p->name[nlen] = '\0';
  p->type = DRM_PROP_RANGE;
  p->range_min = min;
  p->range_max = max;
  p->is_immutable = is_immutable;
  p->enum_count = 0;
  return (uint32_t)id;
}

static uint32_t drm_property_create_enum(const char *name,
                                         const uint64_t *enum_values,
                                         const char *const *enum_names,
                                         int count, bool is_immutable) {
  int id = g_drm_next_prop_id++;
  if (id > DRM_MAX_PROPERTIES) {
    g_drm_next_prop_id = DRM_MAX_PROPERTIES + 1;
    return 0;
  }
  struct drm_property *p = &g_drm_properties[id - 1];
  p->prop_id = (uint32_t)id;
  p->allocated = true;
  __memset(p->name, 0, sizeof(p->name));
  size_t nlen = 0;
  while (name[nlen] && nlen < DRM_PROP_NAME_LEN - 1) {
    p->name[nlen] = name[nlen];
    nlen++;
  }
  p->name[nlen] = '\0';
  p->type = DRM_PROP_ENUM;
  p->is_immutable = is_immutable;
  int copy_count = count < 16 ? count : 16;
  p->enum_count = copy_count;
  for (int i = 0; i < copy_count; i++) {
    p->enums[i].value = enum_values[i];
    __memset(p->enums[i].name, 0, sizeof(p->enums[i].name));
    const char *s = enum_names[i];
    size_t slen = 0;
    while (s[slen] && slen < DRM_PROP_NAME_LEN - 1) {
      p->enums[i].name[slen] = s[slen];
      slen++;
    }
    p->enums[i].name[slen] = '\0';
  }
  return (uint32_t)id;
}

static uint32_t drm_property_create_blob(const char *name, bool is_immutable) {
  int id = g_drm_next_prop_id++;
  if (id > DRM_MAX_PROPERTIES) {
    g_drm_next_prop_id = DRM_MAX_PROPERTIES + 1;
    return 0;
  }
  struct drm_property *p = &g_drm_properties[id - 1];
  p->prop_id = (uint32_t)id;
  p->allocated = true;
  __memset(p->name, 0, sizeof(p->name));
  size_t nlen = 0;
  while (name[nlen] && nlen < DRM_PROP_NAME_LEN - 1) {
    p->name[nlen] = name[nlen];
    nlen++;
  }
  p->name[nlen] = '\0';
  p->type = DRM_PROP_BLOB;
  p->is_immutable = is_immutable;
  return (uint32_t)id;
}

static uint32_t drm_property_create_object(const char *name, uint32_t type,
                                           bool is_immutable) {
  int id = g_drm_next_prop_id++;
  if (id > DRM_MAX_PROPERTIES) {
    g_drm_next_prop_id = DRM_MAX_PROPERTIES + 1;
    return 0;
  }
  struct drm_property *p = &g_drm_properties[id - 1];
  p->prop_id = (uint32_t)id;
  p->allocated = true;
  __memset(p->name, 0, sizeof(p->name));
  size_t nlen = 0;
  while (name[nlen] && nlen < DRM_PROP_NAME_LEN - 1) {
    p->name[nlen] = name[nlen];
    nlen++;
  }
  p->name[nlen] = '\0';
  p->type = DRM_PROP_OBJECT;
  p->is_immutable = is_immutable;
  (void)type;
  return (uint32_t)id;
}

static int drm_property_add_to_object(uint32_t obj_type, uint32_t obj_id,
                                      uint32_t prop_id,
                                      uint64_t initial_value) {
  (void)obj_type;
  /* For now, single object per type: map by obj_id directly.
   * Connector=2, Plane=4, CRTC=1 */
  if (!obj_props_inited) {
    for (int i = 0; i < 8; i++)
      obj_props_tbl[i].lock = SPINLOCK_INIT;
    obj_props_inited = true;
  }
  int idx = (int)obj_id;
  if (idx < 0 || idx >= 8)
    return -EINVAL;
  struct drm_object_props *props = &obj_props_tbl[idx];
  spin_lock(&props->lock);
  if (props->count >= DRM_MAX_PROPS_PER_OBJECT) {
    spin_unlock(&props->lock);
    return -ENOSPC;
  }
  props->prop_ids[props->count] = prop_id;
  props->prop_values[props->count] = initial_value;
  props->count++;
  spin_unlock(&props->lock);
  return 0;
}

static struct drm_object_props *obj_props_get(uint32_t obj_id,
                                              uint32_t obj_type) {
  (void)obj_type;
  if (!obj_props_inited) {
    for (int i = 0; i < 8; i++)
      obj_props_tbl[i].lock = SPINLOCK_INIT;
    obj_props_inited = true;
  }
  int idx = (int)obj_id;
  if (idx < 0 || idx >= 8)
    return NULL;
  return &obj_props_tbl[idx];
}

static uint32_t drm_blob_create(const void *data, size_t length) {
  int id = g_drm_next_blob_id++;
  if (id > DRM_MAX_BLOBS) {
    g_drm_next_blob_id = DRM_MAX_BLOBS + 1;
    return 0;
  }
  struct drm_blob *b = &g_drm_blobs[id - 1];
  b->blob_id = (uint32_t)id;
  b->allocated = true;
  b->refcount = 1;
  b->length = length;
  /* Allocate blob data on a dedicated page (kmalloc may place it in a slab
   * page shared with other objects; a neighbour's overwrite can corrupt
   * the blob's content). Page-level allocation isolates the blob data. */
  size_t alloc_size = (length <= PAGE_SIZE) ? PAGE_SIZE : length;
  b->data = kmalloc(alloc_size);
  if (!b->data) {
    b->allocated = false;
    return 0;
  }
  __memcpy(b->data, data, length);
  return (uint32_t)id;
}

static __attribute__((unused)) void drm_blob_release(uint32_t blob_id) {
  struct drm_blob *b = drm_find_blob(blob_id);
  if (!b)
    return;
  b->refcount--;
  if (b->refcount <= 0) {
    if (b->data)
      kfree(b->data);
    __memset(b, 0, sizeof(*b));
  }
}

static __attribute__((unused)) int
drm_object_prop_set(uint32_t obj_id, const char *name, uint64_t value) {
  struct drm_object_props *props = obj_props_get(obj_id, 0);
  if (!props)
    return -EINVAL;
  spin_lock(&props->lock);
  for (int i = 0; i < props->count; i++) {
    uint32_t pid = props->prop_ids[i];
    struct drm_property *p = drm_find_property(pid);
    if (p && __strncmp(p->name, name, DRM_PROP_NAME_LEN) == 0) {
      props->prop_values[i] = value;
      spin_unlock(&props->lock);
      return 0;
    }
  }
  spin_unlock(&props->lock);
  return -ENOENT;
}

static int drm_alloc_fb_id(void) {
  for (int i = 0; i < MAX_FRAMEBUFFERS; i++) {
    if (g_drm.fbs[i].fb_id == 0) {
      g_drm.fbs[i].fb_id = i + 1; /* fb_id = slot index + 1 */
      g_drm.fbs[i].refcount = 1;
      return g_drm.fbs[i].fb_id;
    }
  }
  return -1;
}

/* DRM_IOCTL_VERSION */
static long drm_ioctl_version(void *arg) {
  struct drm_version *v = (struct drm_version *)arg;
  static const char driver_name[] = "virtio_gpu";
  size_t name_len = sizeof(driver_name) - 1;

  v->version_major = 0;
  v->version_minor = 1;
  v->version_patchlevel = 0;

  /* Second pass: copy driver name to user buffer.
   * v->name is a user-space pointer (copied verbatim by sys_ioctl's
   * copy_from_user). v->name_len is the buffer size libdrm allocated. */
  if (v->name != NULL && v->name_len > 0) {
    size_t copy_len = (name_len < v->name_len - 1) ? name_len : v->name_len - 1;
    if (copy_to_user((void *)(uintptr_t)v->name, driver_name, copy_len))
      return -EFAULT;
    if (copy_len == v->name_len - 1) {
      char nul = '\0';
      if (copy_to_user((void *)(uintptr_t)(v->name + copy_len), &nul, 1))
        return -EFAULT;
    }
  }

  v->name_len = name_len;
  v->date_len = 0;
  v->desc_len = 0;
  return 0;
}

/* DRM_IOCTL_VIRTGPU_GETPARAM — return Venus runtime params.
 * drm_virtgpu_getparam.value is a user-space __u64 pointer; the kernel
 * writes the low 32 bits of the value and zeros the high 32 bits. */
static long drm_ioctl_virtgpu_getparam(void *arg) {
  struct drm_virtgpu_getparam *p = (struct drm_virtgpu_getparam *)arg;
  uint32_t val = 0;

  switch (p->param) {
  case VIRTGPU_PARAM_3D_FEATURES:
    val = 1;
    break;
  case VIRTGPU_PARAM_CAPSET_QUERY_FIX:
    val = 1;
    break;
  case VIRTGPU_PARAM_RESOURCE_BLOB:
    val = 0; /* blob path retired (Venus-only); virgl uses v1 RESOURCE_CREATE */
    break;
  case VIRTGPU_PARAM_HOST_VISIBLE:
    val = 0; /* was a HOST3D blob property; blob path retired */
    break;
  case VIRTGPU_PARAM_CONTEXT_INIT:
    val = 1;
    break;
  case VIRTGPU_PARAM_CROSS_DEVICE:
    val = 0; /* not supported */
    break;
  case VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs:
    /* Bitmask: bit N advertises capset id N (bit1=VIRGL, bit2=VIRGL2).
     * Built from whatever the host advertised; nothing is synthesized, so the
     * mask is 0 when the host exposes no capsets (e.g. no virgl back-end). */
    val = 0;
    spin_lock(&g_drm.capset_lock);
    for (uint32_t i = 0; i < g_drm.num_capsets; i++)
      val |= (1u << g_drm.capsets[i].id);
    spin_unlock(&g_drm.capset_lock);
    break;
  default:
    printk(LOG_WARN, "drm: unknown virtgpu param %llu\n",
           (unsigned long long)p->param);
    val = 0;
    break;
  }

  /* value is a user-space uint64_t pointer; write low 32 bits + zero high 32.
   */
  uint32_t zero_hi = 0;
  if (copy_to_user((void *)(uintptr_t)p->value, &val, sizeof(val)))
    return -EFAULT;
  if (copy_to_user((void *)(uintptr_t)(p->value + sizeof(val)), &zero_hi,
                   sizeof(zero_hi)))
    return -EFAULT;
  printk(LOG_DEBUG, "drm: GETPARAM param=%llu val=%u\n",
         (unsigned long long)p->param, val);
  return 0;
}

/* Forward declarations (plan1: ctx_id pool helpers, defined later). */
static uint32_t alloc_ctx_id(void);
static void free_ctx_id(uint32_t id);

/* virgl legacy (v1) helpers + capset probe, defined later. */
static uint32_t alloc_virgl_handle(void);
static void free_virgl_handle(uint32_t handle);
static struct drm_virgl_resource *drm_find_virgl_resource(uint32_t handle);
static bool virgl_capset_present(uint32_t capset_id);
static bool drm_file_has_handle(struct drm_file *df, int handle, bool virgl);

/* DRM_IOCTL_VIRTGPU_GET_CAPS — return cached capset payload. addr is a
 * user-space pointer; copy up to c->size bytes. Serves any host-cached capset
 * (virgl id=1/2 when the host advertises them). An unknown id returns -EINVAL:
 * the virgl winsys checks errno==EINVAL to fall back from capset 2 (VIRGL2) to
 * capset 1 (VIRGL), so -ENOENT would break that path. */
static long drm_ioctl_virtgpu_get_caps(void *arg) {
  struct drm_virtgpu_get_caps *c = (struct drm_virtgpu_get_caps *)arg;

  const void *data = NULL;
  uint32_t data_size = 0;
  spin_lock(&g_drm.capset_lock);
  for (uint32_t i = 0; i < g_drm.num_capsets; i++) {
    if (g_drm.capsets[i].id == c->cap_set_id) {
      data = g_drm.capsets[i].data;
      data_size = g_drm.capsets[i].size;
      break;
    }
  }
  spin_unlock(&g_drm.capset_lock);
  if (!data)
    return -EINVAL;

  uint32_t copy_size = (c->size < data_size) ? c->size : data_size;
  printk(LOG_DEBUG, "drm: GET_CAPS id=%u requested=%u cached=%u copied=%u\n",
         c->cap_set_id, c->size, data_size, copy_size);
  if (copy_to_user((void *)(uintptr_t)c->addr, data, copy_size))
    return -EFAULT;
  return 0;
}

/* DRM_IOCTL_VIRTGPU_CONTEXT_INIT — translate drm_virtgpu_context_set_param[]
 * into VIRTIO_GPU_CMD_CTX_CREATE. ctx_set_params is a user-space pointer. */
static long drm_ioctl_virtgpu_context_init(void *arg, struct drm_file *df) {
  struct drm_virtgpu_context_init *ci = (struct drm_virtgpu_context_init *)arg;

  if (ci->num_params == 0)
    return -EINVAL;

  struct drm_virtgpu_context_set_param *params =
      kmalloc(ci->num_params * sizeof(*params));
  if (!params)
    return -ENOMEM;
  if (copy_from_user(params, (void *)(uintptr_t)ci->ctx_set_params,
                     ci->num_params * sizeof(*params))) {
    kfree(params);
    return -EFAULT;
  }

  uint32_t capset_id = 0, num_rings = 0, poll_rings_mask = 0;
  bool have_num_rings = false;
  for (uint32_t i = 0; i < ci->num_params; i++) {
    switch (params[i].param) {
    case VIRTGPU_CONTEXT_PARAM_CAPSET_ID:
      capset_id = (uint32_t)params[i].value;
      break;
    case VIRTGPU_CONTEXT_PARAM_NUM_RINGS:
      have_num_rings = true;
      num_rings = (uint32_t)params[i].value;
      break;
    case VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK:
      poll_rings_mask = (uint32_t)params[i].value;
      break;
    case VIRTGPU_CONTEXT_PARAM_DEBUG_NAME:
      break; /* ignored */
    }
  }
  kfree(params);

  /* Accept any capset the host advertised (Venus 4, virgl 1/2). The virgl
   * winsys only sets CAPSET_ID (num_params=1) and never NUM_RINGS, so default
   * to a single ring in that case — EXECBUFFER then accepts ring_idx=0. */
  if (!virgl_capset_present(capset_id))
    return -EINVAL;
  if (!have_num_rings)
    num_rings = 1;
  if (num_rings == 0 || num_rings > 64)
    return -EINVAL;

  uint32_t ctx_id = alloc_ctx_id();
  if (ctx_id == 0) {
    printk(LOG_ERROR, "drm: CONTEXT_INIT context id pool exhausted\n");
    return -ENOMEM;
  }

  uint64_t fence_flags;
  spin_lock_irqsave(&g_drm.fence_lock, &fence_flags);
  __memset(g_drm.completed_fence_ids[ctx_id - 1], 0,
           sizeof(g_drm.completed_fence_ids[ctx_id - 1]));
  spin_unlock_irqrestore(&g_drm.fence_lock, fence_flags);

  struct virtio_gpu_ctx_create cmd;
  __memset(&cmd, 0, sizeof(cmd));
  cmd.hdr.type = VIRTIO_GPU_CMD_CTX_CREATE;
  cmd.hdr.ctx_id = ctx_id;
  cmd.nlen = 0;
  cmd.context_init = capset_id & VIRTIO_GPU_CONTEXT_INIT_CAPSET_ID_MASK;

  struct virtio_gpu_ctrl_hdr_response resp;
  __memset(&resp, 0, sizeof(resp));
  int rc = virtio_gpu_send_cmd_3d(&g_virtio_gpu, &cmd, sizeof(cmd), &resp,
                                  sizeof(resp));
  if (rc || resp.hdr.type != VIRTIO_GPU_RESP_OK_NODATA) {
    printk(LOG_ERROR,
           "drm: CONTEXT_INIT failed ctx=%u capset=%u rc=%d response=0x%x\n",
           ctx_id, capset_id, rc, resp.hdr.type);
    free_ctx_id(ctx_id);
    return rc ? rc : -EIO;
  }

  if (!df) {
    free_ctx_id(ctx_id);
    return -EBADF;
  }
  df->ctx_id = ctx_id;
  df->num_rings = num_rings;
  df->poll_rings_mask = poll_rings_mask;

  df->ring_fence_counters = kmalloc(num_rings * sizeof(uint64_t));
  if (!df->ring_fence_counters) {
    /* rollback: destroy ctx on host + free id */
    struct virtio_gpu_ctx_create destroy;
    __memset(&destroy, 0, sizeof(destroy));
    destroy.hdr.type = VIRTIO_GPU_CMD_CTX_DESTROY;
    destroy.hdr.ctx_id = ctx_id;
    virtio_gpu_send_cmd_3d(&g_virtio_gpu, &destroy, sizeof(destroy), &resp,
                           sizeof(resp));
    free_ctx_id(ctx_id);
    df->ctx_id = 0;
    return -ENOMEM;
  }
  __memset(df->ring_fence_counters, 0, num_rings * sizeof(uint64_t));

  printk(LOG_DEBUG, "drm: CONTEXT_INIT ctx_id=%u rings=%u\n", ctx_id,
         num_rings);
  return 0;
}

/* DRM_IOCTL_VIRTGPU_RESOURCE_CREATE — virgl legacy v1 path. The winsys passes
 * target/format/bind/dims/stride/size in and reads back res_handle (host id)
 * + bo_handle (new GEM). It never calls ATTACH_BACKING separately, so the
 * kernel allocates guest backing and attaches it to the host resource here.
 * The bo_handle→res_handle mapping is persisted in g_drm.virgl_res[] so later
 * TRANSFER_TO/FROM_HOST and WAIT (which pass only bo_handle) can resolve it. */
static long drm_ioctl_virtgpu_resource_create(void *arg, struct drm_file *df) {
  struct drm_virtgpu_resource_create *rc =
      (struct drm_virtgpu_resource_create *)arg;

  if (rc->bo_handle != 0) /* winsys always passes 0 in */
    return -EINVAL;
  if (rc->size == 0 || rc->width == 0 || rc->height == 0)
    return -EINVAL;
  if (rc->flags & ~VIRTIO_GPU_RESOURCE_FLAG_Y_0_TOP)
    return -EINVAL;

  uint32_t handle = alloc_virgl_handle();
  if (handle == 0)
    return -ENOMEM;
  uint32_t res_id = handle; /* host resource id == GEM handle */

  uint32_t npages = (rc->size + PAGE_SIZE - 1) / PAGE_SIZE;
  void *vaddr = bfc_alloc_page_data(npages);
  if (!vaddr) {
    free_virgl_handle(handle);
    return -ENOMEM;
  }
  __memset(vaddr, 0, rc->size);
  uint64_t guest_phys = (uint64_t)PHY_ADDR((uintptr_t)vaddr);

  struct virtio_gpu_resource_create_3d cmd;
  __memset(&cmd, 0, sizeof(cmd));
  cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
  cmd.resource_id = res_id;
  cmd.target = rc->target;
  cmd.format = rc->format;
  cmd.bind = rc->bind;
  cmd.width = rc->width;
  cmd.height = rc->height;
  cmd.depth = rc->depth;
  cmd.array_size = rc->array_size;
  cmd.last_level = rc->last_level;
  cmd.nr_samples = rc->nr_samples;
  cmd.flags = rc->flags;

  struct virtio_gpu_ctrl_hdr_response resp;
  __memset(&resp, 0, sizeof(resp));
  int rc2 = virtio_gpu_send_cmd_3d(&g_virtio_gpu, &cmd, sizeof(cmd), &resp,
                                   sizeof(resp));
  if (rc2 || resp.hdr.type != VIRTIO_GPU_RESP_OK_NODATA) {
    bfc_free_page_data(vaddr, npages);
    free_virgl_handle(handle);
    return rc2 ? rc2 : -EIO;
  }

  if (virtio_gpu_attach_backing(res_id, guest_phys, (uint32_t)rc->size) < 0) {
    virtio_gpu_resource_unref(res_id);
    bfc_free_page_data(vaddr, npages);
    free_virgl_handle(handle);
    return -EIO;
  }

  struct drm_virgl_resource *r = &g_drm.virgl_res[handle - VIRGL_HANDLE_BASE];
  r->bo_handle = handle;
  r->res_handle = res_id;
  r->guest_phys = guest_phys;
  r->kernel_vaddr = vaddr;
  r->size = rc->size;
  r->refcount = 1;
  __memset(r->ctx_attach_bitmap, 0, sizeof(r->ctx_attach_bitmap));

  rc->bo_handle = handle;
  rc->res_handle = res_id;

  if (df && df->created_virgl_count < MAX_VIRGL_RESOURCES)
    df->created_virgl_handles[df->created_virgl_count++] = (int)handle;

  printk(LOG_DEBUG,
         "drm: RESOURCE_CREATE(v1) %ux%ux%u fmt=%u -> bo=%u res=%u\n",
         rc->width, rc->height, rc->depth, rc->format, handle, res_id);
  return 0;
}

/* DRM_IOCTL_VIRTGPU_RESOURCE_INFO — return res_handle/size/blob_mem.
 * virgl legacy (v1) resources only; handles live at/above VIRGL_HANDLE_BASE. */
static long drm_ioctl_virtgpu_resource_info(void *arg) {
  struct drm_virtgpu_resource_info *ri =
      (struct drm_virtgpu_resource_info *)arg;

  struct drm_virgl_resource *r = drm_find_virgl_resource(ri->bo_handle);
  if (!r)
    return -EINVAL;

  ri->res_handle = r->res_handle;
  ri->size = (uint32_t)r->size;
  ri->blob_mem = 0;
  return 0;
}

/* DRM_IOCTL_VIRTGPU_MAP — return an mmap offset for a legacy virgl BO. */
static long drm_ioctl_virtgpu_map(void *arg, struct drm_file *df) {
  struct drm_virtgpu_map *map = (struct drm_virtgpu_map *)arg;
  if (!map || !df)
    return -EFAULT;
  if (!drm_file_has_handle(df, (int)map->handle, true))
    return -ENOENT;

  spin_lock(&g_drm.virgl_lock);
  struct drm_virgl_resource *r = drm_find_virgl_resource(map->handle);
  if (!r) {
    spin_unlock(&g_drm.virgl_lock);
    return -ENOENT;
  }
  map->offset = (uint64_t)map->handle << PAGE_SHIFT;
  spin_unlock(&g_drm.virgl_lock);
  return 0;
}

/* Forward declaration for the plan2 sync_file fd install helper, used by
 * EXECBUFFER's FENCE_FD_OUT but defined later in the file. */
static int drm_fence_install_sync_file(struct drm_fence *fence, xtask *proc);

/* Make a host resource visible to a virgl context before SUBMIT_3D refers to
 * it. Mesa supplies the required GEM handles in EXECBUFFER.bo_handles. */
static int drm_virgl_attach_resource(uint32_t handle, uint32_t ctx_id) {
  uint32_t bit = ctx_id - 1;
  uint32_t word = bit / 32;
  uint32_t mask = 1u << (bit % 32);

  spin_lock(&g_drm.virgl_lock);
  struct drm_virgl_resource *r = drm_find_virgl_resource(handle);
  if (!r) {
    spin_unlock(&g_drm.virgl_lock);
    return -ENOENT;
  }
  if (r->ctx_attach_bitmap[word] & mask) {
    spin_unlock(&g_drm.virgl_lock);
    return 0;
  }
  uint32_t resource_id = r->res_handle;
  spin_unlock(&g_drm.virgl_lock);

  struct virtio_gpu_ctx_resource cmd;
  __memset(&cmd, 0, sizeof(cmd));
  cmd.hdr.type = VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE;
  cmd.hdr.ctx_id = ctx_id;
  cmd.resource_id = resource_id;

  struct virtio_gpu_ctrl_hdr_response resp;
  __memset(&resp, 0, sizeof(resp));
  int rc = virtio_gpu_send_cmd_3d(&g_virtio_gpu, &cmd, sizeof(cmd), &resp,
                                  sizeof(resp));
  if (rc || resp.hdr.type != VIRTIO_GPU_RESP_OK_NODATA)
    return rc ? rc : -EIO;

  spin_lock(&g_drm.virgl_lock);
  r = drm_find_virgl_resource(handle);
  if (!r || r->res_handle != resource_id) {
    spin_unlock(&g_drm.virgl_lock);
    return -ENOENT;
  }
  r->ctx_attach_bitmap[word] |= mask;
  spin_unlock(&g_drm.virgl_lock);

  printk(LOG_INFO, "drm: context %u attached bo=%u resource=%u\n", ctx_id,
         handle, resource_id);
  return 0;
}

static void drm_virgl_forget_context(uint32_t ctx_id) {
  uint32_t bit = ctx_id - 1;
  uint32_t word = bit / 32;
  uint32_t mask = 1u << (bit % 32);

  spin_lock(&g_drm.virgl_lock);
  for (uint32_t i = 0; i < MAX_VIRGL_RESOURCES; i++)
    g_drm.virgl_res[i].ctx_attach_bitmap[word] &= ~mask;
  spin_unlock(&g_drm.virgl_lock);
}

/* Build + send a 3D host transfer (TO/FROM) for a virgl v1 resource. The
 * winsys passes only bo_handle; resolve to the host res_handle here. v1
 * transfers are not context-bound on the host (ctx_id=0); virglrenderer
 * reaches the guest backing via the resource id. */
static long virgl_transfer_host_3d(void *arg, uint32_t cmd_type) {
  struct drm_virtgpu_3d_transfer_to_host *t =
      (struct drm_virtgpu_3d_transfer_to_host *)arg;
  /* drm_virtgpu_3d_transfer_from_host has an identical field layout. */

  struct drm_virgl_resource *r = drm_find_virgl_resource(t->bo_handle);
  if (!r)
    return -ENOENT;

  struct virtio_gpu_transfer_host_3d cmd;
  __memset(&cmd, 0, sizeof(cmd));
  cmd.hdr.type = cmd_type;
  cmd.hdr.ctx_id = 0;
  cmd.box.x = t->box.x;
  cmd.box.y = t->box.y;
  cmd.box.z = t->box.z;
  cmd.box.w = t->box.w;
  cmd.box.h = t->box.h;
  cmd.box.d = t->box.d;
  cmd.offset = t->offset;
  cmd.resource_id = r->res_handle;
  cmd.level = t->level;
  cmd.stride = t->stride;
  cmd.layer_stride = t->layer_stride;

  struct virtio_gpu_ctrl_hdr_response resp;
  __memset(&resp, 0, sizeof(resp));
  int rc = virtio_gpu_send_cmd_3d(&g_virtio_gpu, &cmd, sizeof(cmd), &resp,
                                  sizeof(resp));
  if (rc || resp.hdr.type != VIRTIO_GPU_RESP_OK_NODATA)
    return rc ? rc : -EIO;
  return 0;
}

/* DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST — upload guest backing → host resource. */
static long drm_ioctl_virtgpu_transfer_to_host(void *arg) {
  return virgl_transfer_host_3d(arg, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D);
}

/* DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST — download host resource → guest
 * backing. */
static long drm_ioctl_virtgpu_transfer_from_host(void *arg) {
  return virgl_transfer_host_3d(arg, VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D);
}

/* DRM_IOCTL_VIRTGPU_WAIT — virgl legacy busy/block query. `handle` is a GEM
 * bo_handle. v1 TRANSFERs are synchronous (send_cmd_3d blocks until the host
 * responds), so only in-flight EXECBUFFER (SUBMIT_3D) fences can make a
 * resource busy. Granularity is approximated to the owning context: a resource
 * is reported busy if any unsignaled fence for the current fd's context exists.
 * This is conservative (a different resource's submit may be in flight) but
 * never races — virgl only uses this to decide whether to stall. */
static long drm_ioctl_virtgpu_3d_wait(void *arg, struct drm_file *df) {
  struct drm_virtgpu_3d_wait *w = (struct drm_virtgpu_3d_wait *)arg;
  (void)df;

  bool nowait = w->flags & VIRTGPU_WAIT_NOWAIT;

  for (;;) {
    uint32_t ctx_id;
    uint8_t ring_idx;
    uint64_t fence_id;

    spin_lock(&g_drm.virgl_lock);
    struct drm_virgl_resource *r = drm_find_virgl_resource(w->handle);
    if (!r) {
      spin_unlock(&g_drm.virgl_lock);
      return -ENOENT;
    }
    ctx_id = r->last_ctx_id;
    ring_idx = r->last_ring_idx;
    fence_id = r->last_fence_id;
    spin_unlock(&g_drm.virgl_lock);

    if (ctx_id == 0 || fence_id == 0)
      return 0;

    struct drm_fence *fence = NULL;
    uint64_t flags;
    spin_lock_irqsave(&g_drm.fence_lock, &flags);
    if (ctx_id <= MAX_CTX_IDS && ring_idx < MAX_CTX_RINGS &&
        g_drm.completed_fence_ids[ctx_id - 1][ring_idx] >= fence_id) {
      spin_unlock_irqrestore(&g_drm.fence_lock, flags);
      return 0;
    }
    for (int i = 0; i < MAX_FENCES; i++) {
      if (g_drm.fences[i].ctx_id == ctx_id &&
          g_drm.fences[i].ring_idx == ring_idx &&
          g_drm.fences[i].fence_id == fence_id) {
        fence = &g_drm.fences[i];
        refcount_inc(&fence->refcount);
        break;
      }
    }
    spin_unlock_irqrestore(&g_drm.fence_lock, flags);

    if (nowait) {
      drm_fence_put(fence);
      return -EBUSY;
    }

    /* Completion may reclaim the fence between the completed-id probe and
     * table scan. Retry so the completed-id check observes that completion. */
    if (!fence)
      continue;

    drm_fence_wait(fence, 0);
    drm_fence_put(fence);
  }
}

/* DRM_IOCTL_VIRTGPU_EXECBUFFER — submit a 3D command stream on a ring.
 * Out-fence: optional sync_file fd (FENCE_FD_OUT) signaled when the host
 * completes this submission. */
static long drm_ioctl_virtgpu_execbuffer(void *arg, struct drm_file *df) {
  struct drm_virtgpu_execbuffer *eb = (struct drm_virtgpu_execbuffer *)arg;
  int rc = 0;
  if (!df || df->ctx_id == 0)
    return -EINVAL;
  if (eb->size == 0 || eb->command == 0)
    return -EINVAL;
  if (eb->ring_idx >= df->num_rings)
    return -EINVAL;
  if (eb->flags & VIRTGPU_EXECBUF_FENCE_FD_IN)
    return -EINVAL; /* in-fence fd not supported */

  if (eb->num_bo_handles > MAX_VIRGL_RESOURCES)
    return -EINVAL;
  if (eb->num_bo_handles != 0 && eb->bo_handles == 0)
    return -EINVAL;

  uint32_t *bo_handles = NULL;
  if (eb->num_bo_handles != 0) {
    size_t handles_size = eb->num_bo_handles * sizeof(*bo_handles);
    bo_handles = kmalloc(handles_size);
    if (!bo_handles)
      return -ENOMEM;
    if (copy_from_user(bo_handles, (void *)(uintptr_t)eb->bo_handles,
                       handles_size)) {
      kfree(bo_handles);
      return -EFAULT;
    }

    /* Reject handles which aren't present in this DRM file before changing
     * host context state. This also excludes non-virgl GEM objects. */
    for (uint32_t i = 0; i < eb->num_bo_handles; i++) {
      if (!drm_file_has_handle(df, (int)bo_handles[i], true)) {
        kfree(bo_handles);
        return -ENOENT;
      }
      spin_lock(&g_drm.virgl_lock);
      bool exists = drm_find_virgl_resource(bo_handles[i]) != NULL;
      spin_unlock(&g_drm.virgl_lock);
      if (!exists) {
        kfree(bo_handles);
        return -ENOENT;
      }
    }

    for (uint32_t i = 0; i < eb->num_bo_handles; i++) {
      int rc = drm_virgl_attach_resource(bo_handles[i], df->ctx_id);
      if (rc) {
        kfree(bo_handles);
        return rc;
      }
    }
  }

  uint64_t fence_id = ++df->ring_fence_counters[eb->ring_idx];

  /* Copy user command stream into the SUBMIT_3D buffer. */
  void *cmd_data = kmalloc(eb->size);
  if (!cmd_data)
    goto err_free_handles;
  if (copy_from_user(cmd_data, (void *)(uintptr_t)eb->command, eb->size)) {
    kfree(cmd_data);
    rc = -EFAULT;
    goto err_free_handles;
  }

  size_t total_cmd_size = sizeof(struct virtio_gpu_cmd_submit) + eb->size;
  void *submit_buf = kmalloc(total_cmd_size);
  if (!submit_buf) {
    kfree(cmd_data);
    rc = -ENOMEM;
    goto err_free_handles;
  }
  struct virtio_gpu_cmd_submit *sh = (struct virtio_gpu_cmd_submit *)submit_buf;
  __memset(sh, 0, sizeof(*sh));
  sh->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
  sh->hdr.flags = VIRTIO_GPU_FLAG_FENCE | VIRTIO_GPU_FLAG_INFO_RING_IDX;
  sh->hdr.fence_id = fence_id;
  sh->hdr.ctx_id = df->ctx_id;
  sh->hdr.ring_idx = eb->ring_idx;
  sh->size = eb->size;
  __memcpy((char *)submit_buf + sizeof(*sh), cmd_data, eb->size);
  kfree(cmd_data);

  /* Create fence BEFORE submit so the ISR can find it on completion. */
  struct drm_fence *fence =
      drm_fence_create(df->ctx_id, eb->ring_idx, fence_id);
  if (!fence) {
    kfree(submit_buf);
    rc = -ENOMEM;
    goto err_free_handles;
  }

  /* Reserve a reference for the async completion before publishing the
   * descriptor. The creator's reference remains live until out-fence setup is
   * finished, even if the host completes immediately on another CPU. */
  uint64_t fence_flags;
  spin_lock_irqsave(&g_drm.fence_lock, &fence_flags);
  refcount_inc(&fence->refcount);
  spin_unlock_irqrestore(&g_drm.fence_lock, fence_flags);

  /* Submit async: send_cmd_3d_async copies submit_buf/resp into heap nodes
   * it owns, so freeing submit_buf here is safe. */
  struct virtio_gpu_ctrl_hdr_response resp_template;
  __memset(&resp_template, 0, sizeof(resp_template));
  rc = virtio_gpu_send_cmd_3d_async(&g_virtio_gpu, submit_buf, total_cmd_size,
                                    &resp_template, sizeof(resp_template),
                                    fence_id, eb->ring_idx, df->ctx_id);
  kfree(submit_buf);
  if (rc) {
    drm_fence_put(fence); /* unused async-completion reference */
    drm_fence_put(fence);
    goto err_free_handles;
  }

  spin_lock(&g_drm.virgl_lock);
  for (uint32_t i = 0; i < eb->num_bo_handles; i++) {
    struct drm_virgl_resource *r = drm_find_virgl_resource(bo_handles[i]);
    if (!r)
      continue;
    r->last_ctx_id = df->ctx_id;
    r->last_ring_idx = eb->ring_idx;
    r->last_fence_id = fence_id;
  }
  spin_unlock(&g_drm.virgl_lock);
  kfree(bo_handles);
  bo_handles = NULL;

  /* Out-fence sync_file fd: install a fd bound to this fence (takes a ref). */
  if (eb->flags & VIRTGPU_EXECBUF_FENCE_FD_OUT) {
    int fd = drm_fence_install_sync_file(fence, current_task);
    if (fd < 0) {
      /* fence still valid and will signal; just no fd. Report error. */
      drm_fence_put(fence); /* creator reference */
      return fd;
    }
    eb->fence_fd = fd;
  }

  /* The ISR owns the async-completion reference; an optional sync_file owns
   * another. Drop the creator reference now that fd installation cannot race
   * completion-driven reclamation. */
  drm_fence_put(fence);

  printk(LOG_DEBUG, "drm: EXECBUFFER ring=%u fence_id=%llu size=%u -> fd=%d\n",
         eb->ring_idx, (unsigned long long)fence_id, eb->size, eb->fence_fd);
  return 0;

err_free_handles:
  kfree(bo_handles);
  return rc;
}

/* DRM_IOCTL_GET_CAP */
static long drm_ioctl_get_cap(void *arg) {
  struct drm_get_cap *c = (struct drm_get_cap *)arg;
  switch (c->capability) {
  case DRM_CAP_DUMB_BUFFER:
    c->value = 1;
    return 0;
  case DRM_CAP_DUMB_PREFERRED_DEPTH:
    c->value = 24;
    return 0;
  case DRM_CAP_DUMB_PREFER_SHADOW:
    c->value = 0;
    return 0;
  case DRM_CAP_VBLANK_HIGH_CRTC:
    c->value = 0;
    return 0;
  case DRM_CAP_PRIME:
    c->value = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT;
    return 0;
  case 0x0D:      /* DRM_CAP_ATOMIC */
    c->value = 0; /* force legacy path */
    return 0;
  case DRM_CAP_TIMESTAMP_MONOTONIC:
    c->value = 1;
    return 0;
  case DRM_CAP_ASYNC_PAGE_FLIP:
    c->value = 0;
    return 0;
  case 0x10:
    c->value = 0;
    return 0; /* DRM_CAP_ADDFB2_MODIFIERS */
  case DRM_CAP_CRTC_IN_VBLANK_EVENT:
    c->value = 1;
    return 0;
  default:
    return -EINVAL;
  }
}

/* DRM_IOCTL_SET_CLIENT_CAP */
static long drm_ioctl_set_client_cap(void *arg) {
  struct drm_set_client_cap *c = (struct drm_set_client_cap *)arg;
  switch (c->capability) {
  case DRM_CLIENT_CAP_UNIVERSAL_PLANES:
    c->value = 1;
    return 0;
  case DRM_CLIENT_CAP_ATOMIC:
    return -EINVAL; /* not supported */
  default:
    return -EINVAL;
  }
}

/* DROP_MASTER 清理 — 重置 master 相关状态 */
static void drm_master_cleanup(void) {
  /* 1. Clear current FB (unbind CRTC scanout) */
  if (g_drm.current_fb_id != 0) {
    g_drm.current_fb_id = 0;
  }

  /* 2. Clear pending page flip event */
  spin_lock(&g_drm.event_lock);
  g_drm.event_pending = false;
  g_drm.event_sequence = 0;
  g_drm.event_user_data = 0;
  spin_unlock(&g_drm.event_lock);

  /* 3. Disable cursor */
  extern struct drm_cursor g_drm_cursor;
  g_drm_cursor.enabled = false;
  g_drm_cursor.dirty = false;
}

/* Forward declaration (defined later in per-fd section) */
/* DRM_IOCTL_SET_MASTER — per-fd 互斥 */
static long drm_ioctl_set_master(struct drm_file *f) {
  if (!f)
    return -EBADF;
  if (f->is_render)
    return -EACCES;

  spin_lock(&g_drm_files_lock);
  if (f->is_master) {
    /* Already master: idempotent */
    spin_unlock(&g_drm_files_lock);
    return 0;
  }

  /* Check if any other fd holds master */
  for (int i = 0; i < MAX_DRM_FDS; i++) {
    if (g_drm_files[i].used && g_drm_files[i].is_master) {
      spin_unlock(&g_drm_files_lock);
      return -EBUSY;
    }
  }

  f->is_master = true;
  g_drm.is_master = true;
  spin_unlock(&g_drm_files_lock);
  return 0;
}

static long drm_ioctl_drop_master(struct drm_file *f) {
  if (!f)
    return -EBADF;
  if (f->is_render)
    return -EACCES;

  spin_lock(&g_drm_files_lock);
  if (!f->is_master) {
    spin_unlock(&g_drm_files_lock);
    return -EPERM;
  }
  f->is_master = false;
  g_drm.is_master = false;
  spin_unlock(&g_drm_files_lock);

  drm_master_cleanup();
  return 0;
}

/* DRM_IOCTL_GET_MAGIC — 记录到 per-fd */
static long drm_ioctl_get_magic(void *arg, struct drm_file *f) {
  struct drm_auth *a = (struct drm_auth *)arg;
  if (!a)
    return -EFAULT;
  if (!f)
    return -EBADF;
  if (f->is_render)
    return -EACCES;

  a->magic = ++g_drm.magic_counter;
  f->authenticated_magic = a->magic;
  f->auth_valid = false; /* not yet authenticated */
  return 0;
}

/* DRM_IOCTL_AUTH_MAGIC — 严格校验：仅 master fd 可认证，且 magic 必须是已签发的
 */
static long drm_ioctl_auth_magic(void *arg, struct drm_file *current) {
  struct drm_auth *a = (struct drm_auth *)arg;
  if (!a)
    return -EFAULT;
  if (!current)
    return -EBADF;
  if (current->is_render)
    return -EACCES;

  spin_lock(&g_drm_files_lock);
  /* Only the master fd can authenticate magics */
  if (!current->is_master) {
    spin_unlock(&g_drm_files_lock);
    return -EPERM;
  }

  /* Search all open fds for matching magic */
  for (int i = 0; i < MAX_DRM_FDS; i++) {
    if (g_drm_files[i].used && g_drm_files[i].authenticated_magic == a->magic) {
      g_drm_files[i].auth_valid = true;
      spin_unlock(&g_drm_files_lock);
      return 0;
    }
  }
  spin_unlock(&g_drm_files_lock);
  return -EPERM;
}

/* DRM_IOCTL_MODE_GETPROPERTY */
static long drm_ioctl_getproperty(void *arg) {
  struct drm_mode_get_property *p = (struct drm_mode_get_property *)arg;
  if (!p)
    return -EFAULT;
  struct drm_property *prop = drm_find_property(p->prop_id);
  if (!prop)
    return -ENOENT;

  __memset(p->name, 0, sizeof(p->name));
  __memcpy(p->name, prop->name, DRM_PROP_NAME_LEN);
  p->flags = prop->is_immutable ? DRM_MODE_PROP_IMMUTABLE : 0;

  switch (prop->type) {
  case DRM_PROP_RANGE:
    p->flags |= DRM_MODE_PROP_RANGE;
    if (p->values_ptr) {
      uint64_t vals[2] = {prop->range_min, prop->range_max};
      if (copy_to_user((void *)(uintptr_t)p->values_ptr, vals, sizeof(vals)))
        return -EFAULT;
      p->count_values = 2;
    }
    break;
  case DRM_PROP_ENUM:
    p->flags |= DRM_MODE_PROP_ENUM;
    p->count_values = 0;
    if (p->enum_blob_ptr) {
      for (int i = 0; i < prop->enum_count; i++) {
        struct drm_mode_property_enum e;
        e.value = prop->enums[i].value;
        __memset(e.name, 0, sizeof(e.name));
        __memcpy(e.name, prop->enums[i].name, DRM_PROP_NAME_LEN);
        if (copy_to_user((void *)(uintptr_t)(p->enum_blob_ptr + i * sizeof(e)),
                         &e, sizeof(e)))
          return -EFAULT;
      }
    }
    p->count_enum_blobs = prop->enum_count;
    break;
  case DRM_PROP_BLOB:
    p->flags |= DRM_MODE_PROP_BLOB;
    break;
  case DRM_PROP_OBJECT:
    p->flags |= DRM_MODE_PROP_OBJECT;
    p->count_values = 0;
    break;
  }
  return 0;
}

/* DRM_IOCTL_MODE_GETPROPBLOB */
static long drm_ioctl_getpropblob(void *arg) {
  struct drm_mode_get_blob *b = (struct drm_mode_get_blob *)arg;
  if (!b)
    return -EFAULT;
  struct drm_blob *blob = drm_find_blob(b->blob_id);
  if (!blob)
    return -ENOENT;

  b->length = (uint32_t)blob->length;
  if (b->data && b->length > 0) {
    if (copy_to_user((void *)(uintptr_t)b->data, blob->data, b->length))
      return -EFAULT;
  }
  return 0;
}

/* DRM_IOCTL_MODE_OBJ_GETPROPERTIES */
static long drm_ioctl_obj_getproperties(void *arg) {
  struct drm_mode_obj_get_properties *o =
      (struct drm_mode_obj_get_properties *)arg;
  if (!o)
    return -EFAULT;
  struct drm_object_props *props = obj_props_get(o->obj_id, o->obj_type);
  if (!props)
    return -EINVAL;

  spin_lock(&props->lock);
  o->count_props = props->count;

  if (o->props_ptr && o->prop_values_ptr) {
    if (copy_to_user((void *)(uintptr_t)o->props_ptr, props->prop_ids,
                     props->count * sizeof(uint32_t))) {
      spin_unlock(&props->lock);
      return -EFAULT;
    }
    if (copy_to_user((void *)(uintptr_t)o->prop_values_ptr, props->prop_values,
                     props->count * sizeof(uint64_t))) {
      spin_unlock(&props->lock);
      return -EFAULT;
    }
  }
  spin_unlock(&props->lock);
  return 0;
}

static long drm_set_object_property(uint32_t obj_id, uint32_t obj_type,
                                    uint32_t prop_id, uint64_t value) {
  struct drm_property *prop = drm_find_property(prop_id);
  if (!prop)
    return -ENOENT;
  if (prop->is_immutable)
    return -EINVAL;

  if (prop->type == DRM_PROP_RANGE &&
      (value < prop->range_min || value > prop->range_max))
    return -EINVAL;
  if (prop->type == DRM_PROP_ENUM) {
    bool found = false;
    for (int i = 0; i < prop->enum_count; i++)
      found |= prop->enums[i].value == value;
    if (!found)
      return -EINVAL;
  }

  struct drm_object_props *props = obj_props_get(obj_id, obj_type);
  if (!props)
    return -ENOENT;
  spin_lock(&props->lock);
  for (int i = 0; i < props->count; i++) {
    if (props->prop_ids[i] != prop_id)
      continue;
    props->prop_values[i] = value;
    spin_unlock(&props->lock);
    return 0;
  }
  spin_unlock(&props->lock);
  return -ENOENT;
}

static long drm_ioctl_setproperty(void *arg) {
  struct drm_mode_connector_set_property *set = arg;
  if (!set)
    return -EFAULT;
  if (set->connector_id != DRM_CONNECTOR_ID)
    return -ENOENT;
  return drm_set_object_property(set->connector_id, DRM_MODE_OBJECT_CONNECTOR,
                                 set->prop_id, set->value);
}

static long drm_ioctl_obj_setproperty(void *arg) {
  struct drm_mode_obj_set_property *set = arg;
  if (!set)
    return -EFAULT;
  return drm_set_object_property(set->obj_id, set->obj_type, set->prop_id,
                                 set->value);
}

/* ===== EDID generation (Phase C) ===== */

struct edid_block {
  uint8_t header[8];
  uint16_t id_manufacturer;
  uint16_t id_product_code;
  uint32_t id_serial;
  uint8_t week_of_manufacture;
  uint8_t year_of_manufacture;
  uint8_t edid_version;
  uint8_t edid_revision;

  uint8_t video_input_def;
  uint8_t max_horizontal_cm;
  uint8_t max_vertical_cm;
  uint8_t gamma;
  uint8_t features;

  uint8_t chroma[10];
  uint8_t established[3];
  uint8_t standard_timings[16];

  struct {
    uint16_t pixel_clock;
    uint8_t h_active_lo;
    uint8_t h_blank_lo;
    uint8_t h_active_hi_blank_hi;
    uint8_t v_active_lo;
    uint8_t v_blank_lo;
    uint8_t v_active_hi_blank_hi;
    uint8_t h_sync_offset_lo;
    uint8_t h_sync_pulse_lo;
    uint8_t vsync_offset_lo_pulse_lo;
    uint8_t hvsync_hi;
    uint8_t h_image_size_lo;
    uint8_t v_image_size_lo;
    uint8_t image_size_hi;
    uint8_t h_border;
    uint8_t v_border;
    uint8_t flags;
  } __attribute__((packed)) detailed_timings[4];

  uint8_t extension_flag;
  uint8_t checksum;
} __attribute__((packed));

static void drm_generate_edid(uint8_t *buf, size_t bufsz, uint32_t width,
                              uint32_t height) {
  if (bufsz < 128)
    return;

  struct edid_block *e = (struct edid_block *)buf;
  __memset(e, 0, 128);

  /* 1. Header */
  e->header[0] = 0x00;
  e->header[1] = 0xFF;
  e->header[2] = 0xFF;
  e->header[3] = 0xFF;
  e->header[4] = 0xFF;
  e->header[5] = 0xFF;
  e->header[6] = 0xFF;
  e->header[7] = 0x00;

  /* 2. Manufacturer: "VBO" (close enough to VirtualBox PNP) */
  e->id_manufacturer = 0x0914;
  e->id_product_code = 0x0001;
  e->edid_version = 1;
  e->edid_revision = 3;
  e->video_input_def = 0x80; /* digital */
  e->features = 0x06;        /* RGB, preferred timing mode */

  /* 3. Detailed Timing Descriptor (Descriptor #1) */
  uint32_t total_h = width + 160;
  uint32_t total_v = height + 50;
  uint32_t clock_khz = total_h * total_v * 60 / 1000;
  uint32_t pixel_clock_10khz = (clock_khz + 5000) / 10000;
  if (pixel_clock_10khz > 65535)
    pixel_clock_10khz = 65535;

  e->detailed_timings[0].pixel_clock = (uint16_t)pixel_clock_10khz;

  e->detailed_timings[0].h_active_lo = width & 0xFF;
  e->detailed_timings[0].h_blank_lo = 160 & 0xFF;
  e->detailed_timings[0].h_active_hi_blank_hi =
      ((width >> 8) & 0xF) | (((160 >> 8) & 0xF) << 4);

  e->detailed_timings[0].v_active_lo = height & 0xFF;
  e->detailed_timings[0].v_blank_lo = 50 & 0xFF;
  e->detailed_timings[0].v_active_hi_blank_hi =
      ((height >> 8) & 0xF) | (((50 >> 8) & 0xF) << 4);

  uint16_t h_front_porch = 16;
  uint16_t h_sync_width = 32;
  e->detailed_timings[0].h_sync_offset_lo = h_front_porch & 0xFF;
  e->detailed_timings[0].h_sync_pulse_lo = h_sync_width & 0xFF;

  uint8_t v_front_porch = 1;
  uint8_t v_sync_width = 3;
  e->detailed_timings[0].vsync_offset_lo_pulse_lo =
      (v_front_porch & 0xF) | ((v_sync_width & 0xF) << 4);

  e->detailed_timings[0].hvsync_hi =
      ((h_front_porch >> 4) & 0x3) | (((h_sync_width >> 4) & 0x3) << 2) |
      (((v_front_porch >> 4) & 0x3) << 4) | (((v_sync_width >> 4) & 0x3) << 6);

  uint32_t h_mm = width * 254 / 960;
  uint32_t v_mm = height * 254 / 960;
  e->detailed_timings[0].h_image_size_lo = h_mm & 0xFF;
  e->detailed_timings[0].v_image_size_lo = v_mm & 0xFF;
  e->detailed_timings[0].image_size_hi =
      ((h_mm >> 8) & 0xF) | ((v_mm >> 8) & 0xF << 4);

  e->detailed_timings[0].flags = 0x00;

  /* 4. Descriptor #2: Monitor name ("Virtual OS") */
  uint8_t *desc2 = (uint8_t *)&e->detailed_timings[1];
  desc2[0] = desc2[1] = desc2[2] = 0x00;
  desc2[3] = 0xFC;
  const char vname[] = "Virtual OS";
  for (int i = 0; i < 13; i++)
    desc2[4 + i] = (i < (int)sizeof(vname) - 1) ? vname[i] : ' ';

  /* 5. Descriptor #3: Monitor range limits */
  uint8_t *desc3 = (uint8_t *)&e->detailed_timings[2];
  desc3[0] = desc3[1] = desc3[2] = 0x00;
  desc3[3] = 0xFD;
  desc3[4] = 56;
  desc3[5] = 61;
  desc3[6] = 30;
  desc3[7] = 80;
  desc3[8] = 100;

  /* 6. Descriptor #4: Serial number placeholder */
  uint8_t *desc4 = (uint8_t *)&e->detailed_timings[3];
  desc4[0] = desc4[1] = desc4[2] = 0x00;
  desc4[3] = 0xFF;
  const char serial[] = "00000001";
  for (int i = 0; i < 13; i++)
    desc4[4 + i] = (i < (int)sizeof(serial) - 1) ? serial[i] : ' ';

  /* 7. Checksum */
  uint8_t sum = 0;
  for (int i = 0; i < 127; i++)
    sum += buf[i];
  buf[127] = (uint8_t)(256 - sum);
}

/* Fill m->name with "<w>x<h>" (no refresh suffix; buffer is
 * DRM_DISPLAY_INFO_LEN). */
static void drm_mode_fill_name(struct drm_mode_modeinfo *m) {
  uint32_t w = g_drm.fb_width, h = g_drm.fb_height;
  char buf[16];
  int n = 0;
  char tmp[10];
  int i;
  /* width */
  i = 0;
  if (w == 0)
    tmp[i++] = '0';
  while (w) {
    tmp[i++] = '0' + (w % 10);
    w /= 10;
  }
  while (i)
    buf[n++] = tmp[--i];
  buf[n++] = 'x';
  /* height */
  i = 0;
  if (h == 0)
    tmp[i++] = '0';
  while (h) {
    tmp[i++] = '0' + (h % 10);
    h /= 10;
  }
  while (i)
    buf[n++] = tmp[--i];
  buf[n] = '\0';
  __memset(m->name, 0, sizeof(m->name));
  __memcpy(m->name, buf, n + 1);
}

/* DRM_IOCTL_MODE_GETRESOURCES */
static long drm_ioctl_getresources(void *arg) {
  struct drm_mode_card_res *r = (struct drm_mode_card_res *)arg;
  printk(LOG_DEBUG, "drm_getresources: fb_w=%u fb_h=%u\n", g_drm.fb_width,
         g_drm.fb_height);
  r->count_crtcs = 1;
  r->count_connectors = 1;
  r->count_encoders = 1;
  r->min_width = g_drm.fb_width;
  r->max_width = g_drm.fb_width;
  r->min_height = g_drm.fb_height;
  r->max_height = g_drm.fb_height;

  /* Fill ID buffers (second ioctl call, after libdrm allocates buffers) */
  if (r->crtc_id_ptr) {
    uint32_t id = DRM_CRTC_ID;
    if (copy_to_user((void *)(uintptr_t)r->crtc_id_ptr, &id, sizeof(id)))
      return -EFAULT;
  }
  if (r->connector_id_ptr) {
    uint32_t id = DRM_CONNECTOR_ID;
    if (copy_to_user((void *)(uintptr_t)r->connector_id_ptr, &id, sizeof(id)))
      return -EFAULT;
  }
  if (r->encoder_id_ptr) {
    uint32_t id = DRM_ENCODER_ID;
    if (copy_to_user((void *)(uintptr_t)r->encoder_id_ptr, &id, sizeof(id)))
      return -EFAULT;
  }

  /* count_fbs + fb_id_ptr fill — B-1 fix */
  spin_lock(&g_drm.fb_lock);

  int count = 0;
  for (int i = 0; i < MAX_FRAMEBUFFERS; i++) {
    if (g_drm.fbs[i].fb_id != 0)
      count++;
  }
  r->count_fbs = count;

  /* Fill fb ID buffer (second ioctl call) */
  if (count > 0 && r->fb_id_ptr) {
    uint32_t *fb_buf = (uint32_t *)kmalloc(count * sizeof(uint32_t));
    if (!fb_buf) {
      spin_unlock(&g_drm.fb_lock);
      return -ENOMEM;
    }
    int idx = 0;
    for (int i = 0; i < MAX_FRAMEBUFFERS; i++) {
      if (g_drm.fbs[i].fb_id != 0)
        fb_buf[idx++] = g_drm.fbs[i].fb_id;
    }
    spin_unlock(&g_drm.fb_lock);

    if (copy_to_user((void *)(uintptr_t)r->fb_id_ptr, fb_buf,
                     count * sizeof(uint32_t))) {
      kfree(fb_buf);
      return -EFAULT;
    }
    kfree(fb_buf);
  } else {
    spin_unlock(&g_drm.fb_lock);
  }
  return 0;
}

/* DRM_IOCTL_MODE_GETCRTC */
static long drm_ioctl_getcrtc(void *arg) {
  struct drm_mode_crtc *c = (struct drm_mode_crtc *)arg;
  if (c->crtc_id != DRM_CRTC_ID)
    return -EINVAL;
  c->fb_id = g_drm.current_fb_id;
  c->x = 0;
  c->y = 0;
  c->mode_valid = g_drm.mode_valid ? 1 : 0;
  if (g_drm.mode_valid) {
    struct drm_mode_modeinfo *m = &c->mode;
    __memset(m, 0, sizeof(*m));
    m->clock = 40000;
    m->hdisplay = g_drm.fb_width;
    m->hsync_start = g_drm.fb_width + 16;
    m->hsync_end = g_drm.fb_width + 32;
    m->htotal = g_drm.fb_width + 48;
    m->vdisplay = g_drm.fb_height;
    m->vsync_start = g_drm.fb_height + 1;
    m->vsync_end = g_drm.fb_height + 4;
    m->vtotal = g_drm.fb_height + 10;
    m->vrefresh = 60;
    m->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
    drm_mode_fill_name(m);
  }
  return 0;
}

/* DRM_IOCTL_MODE_SETCRTC */
static long drm_ioctl_setcrtc(void *arg) {
  struct drm_mode_crtc *c = (struct drm_mode_crtc *)arg;
  if (c->crtc_id != DRM_CRTC_ID)
    return -EINVAL;
  if (!c->mode_valid) {
    g_drm.mode_valid = false;
    return 0;
  }
  struct drm_framebuffer *fb = drm_find_fb(c->fb_id);
  if (!fb)
    return -EINVAL;
  g_drm.current_fb_id = c->fb_id;
  g_drm.mode_valid = true;
  struct drm_dumb_buffer *d = drm_find_dumb(fb->dumb_handle);
  uint32_t resource_id = d ? d->virtio_res_id : 0;
  if (fb->is_virgl) {
    struct drm_virgl_resource *r =
        drm_find_virgl_resource((uint32_t)fb->dumb_handle);
    resource_id = r ? r->res_handle : 0;
  }
  if (!resource_id)
    return -EINVAL;
  virtio_gpu_set_scanout(0, resource_id, 0, 0, fb->width, fb->height);
  return 0;
}

/* DRM_IOCTL_MODE_GETCONNECTOR */
static long drm_ioctl_getconnector(void *arg) {
  struct drm_mode_get_connector *c = (struct drm_mode_get_connector *)arg;
  if (c->connector_id != DRM_CONNECTOR_ID)
    return -EINVAL;
  c->connector_type = DRM_MODE_CONNECTOR_VIRTUAL;
  c->connector_type_id = 1;
  c->connection = 1; /* drm_connector_status_connected */
  c->mm_width = 0;
  c->mm_height = 0;
  c->subpixel = 0;
  c->encoder_id = DRM_ENCODER_ID;
  c->count_encoders = 1;
  c->count_modes = 1;
  c->count_props = 2;
  if (c->props_ptr && c->prop_values_ptr) {
    /* Find DPMS and EDID prop_ids dynamically */
    uint32_t props[2];
    uint64_t values[2];
    int found = 0;
    for (int i = 0; i < DRM_MAX_PROPERTIES; i++) {
      if (!g_drm_properties[i].allocated)
        continue;
      if (__strncmp(g_drm_properties[i].name, "DPMS", 5) == 0) {
        props[found] = g_drm_properties[i].prop_id;
        values[found] = DRM_MODE_DPMS_ON;
        found++;
      } else if (__strncmp(g_drm_properties[i].name, "EDID", 5) == 0) {
        props[found] = g_drm_properties[i].prop_id;
        /* Find the EDID blob value from obj_props */
        struct drm_object_props *op =
            obj_props_get(DRM_CONNECTOR_ID, DRM_MODE_OBJECT_CONNECTOR);
        if (op) {
          spin_lock(&op->lock);
          for (int j = 0; j < op->count; j++) {
            struct drm_property *pp = drm_find_property(op->prop_ids[j]);
            if (pp && __strncmp(pp->name, "EDID", 5) == 0) {
              values[found] = op->prop_values[j];
              break;
            }
          }
          spin_unlock(&op->lock);
        }
        found++;
      }
      if (found >= 2)
        break;
    }
    if (found == 2) {
      if (copy_to_user((void *)(uintptr_t)c->props_ptr, props, sizeof(props)) ||
          copy_to_user((void *)(uintptr_t)c->prop_values_ptr, values,
                       sizeof(values)))
        return -EFAULT;
    }
  }

  /* Fill mode data buffer (second ioctl call).
     Always report the default/configured mode as the connector's native
     capability, regardless of whether the CRTC has been set via SETCRTC
     yet. */
  if (c->modes_ptr) {
    struct drm_mode_modeinfo km;
    __memset(&km, 0, sizeof(km));
    km.clock = 40000;
    km.hdisplay = g_drm.fb_width;
    km.hsync_start = g_drm.fb_width + 16;
    km.hsync_end = g_drm.fb_width + 32;
    km.htotal = g_drm.fb_width + 48;
    km.vdisplay = g_drm.fb_height;
    km.vsync_start = g_drm.fb_height + 1;
    km.vsync_end = g_drm.fb_height + 4;
    km.vtotal = g_drm.fb_height + 10;
    km.vrefresh = 60;
    km.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
    drm_mode_fill_name(&km);
    if (copy_to_user((void *)(uintptr_t)c->modes_ptr, &km, sizeof(km)))
      return -EFAULT;
  }

  /* Fill encoder ID buffer (second ioctl call) */
  if (c->encoders_ptr) {
    uint32_t eid = DRM_ENCODER_ID;
    if (copy_to_user((void *)(uintptr_t)c->encoders_ptr, &eid, sizeof(eid)))
      return -EFAULT;
  }
  return 0;
}

/* DRM_IOCTL_MODE_GETENCODER */
static long drm_ioctl_getencoder(void *arg) {
  struct drm_mode_get_encoder *e = (struct drm_mode_get_encoder *)arg;
  if (e->encoder_id != DRM_ENCODER_ID)
    return -EINVAL;
  e->encoder_type = DRM_MODE_ENCODER_VIRTUAL;
  e->crtc_id = DRM_CRTC_ID;
  e->possible_crtcs = 1;
  e->possible_clones = 0;
  return 0;
}

/* DRM_IOCTL_MODE_GETPLANERESOURCES */
static long drm_ioctl_getplaneres(void *arg) {
  struct drm_mode_get_plane_res *p = (struct drm_mode_get_plane_res *)arg;
  p->count_planes = 1;
  if (p->plane_id_ptr) {
    uint32_t id = DRM_PLANE_ID;
    if (copy_to_user((void *)(uintptr_t)p->plane_id_ptr, &id, sizeof(id)))
      return -EFAULT;
  }
  return 0;
}

/* DRM_IOCTL_MODE_GETPLANE */
static long drm_ioctl_getplane(void *arg) {
  struct drm_mode_get_plane *p = (struct drm_mode_get_plane *)arg;
  if (p->plane_id != DRM_PLANE_ID)
    return -EINVAL;
  p->crtc_id = DRM_CRTC_ID;
  p->fb_id = g_drm.current_fb_id;
  p->possible_crtcs = 1;
  p->count_format_types = 4;
  if (p->format_type_ptr) {
    uint32_t fmts[4] = {
        DRM_FORMAT_XRGB8888,
        DRM_FORMAT_ARGB8888,
        DRM_FORMAT_XBGR8888,
        DRM_FORMAT_ABGR8888,
    };
    if (copy_to_user((void *)(uintptr_t)p->format_type_ptr, fmts, sizeof(fmts)))
      return -EFAULT;
  }
  p->gamma_size = 0;
  return 0;
}

/* DRM_IOCTL_MODE_CREATE_DUMB */
static long drm_ioctl_create_dumb(void *arg, struct drm_file *cf) {
  struct drm_mode_create_dumb *d = (struct drm_mode_create_dumb *)arg;
  if (d->width != g_drm.fb_width || d->height != g_drm.fb_height ||
      d->bpp != g_drm.fb_bpp)
    return -EINVAL;
  d->pitch = g_drm.fb_pitch;
  d->size = (uint64_t)g_drm.fb_pitch * g_drm.fb_height;

  spin_lock(&g_drm.dumb_lock);
  int handle = drm_alloc_dumb_handle();
  if (handle < 0) {
    spin_unlock(&g_drm.dumb_lock);
    return -ENOMEM;
  }
  struct drm_dumb_buffer *buf = &g_drm.dumbs[handle - 1];
  spin_unlock(&g_drm.dumb_lock);

  buf->width = d->width;
  buf->height = d->height;
  buf->pitch = d->pitch;
  buf->size = d->size;

  uint32_t npages = (d->size + PAGE_SIZE - 1) / PAGE_SIZE;
  buf->kernel_vaddr = bfc_alloc_page_data(npages);
  if (!buf->kernel_vaddr)
    return -ENOMEM;
  __memset(buf->kernel_vaddr, 0, d->size);
  buf->guest_phys = (uint64_t)PHY_ADDR((uintptr_t)buf->kernel_vaddr);

  buf->virtio_res_id = (uint32_t)handle;
  if (virtio_gpu_create_2d(buf->virtio_res_id, d->width, d->height,
                           VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM) < 0) {
    return -EIO;
  }
  if (virtio_gpu_attach_backing(buf->virtio_res_id, buf->guest_phys, d->size) <
      0) {
    return -EIO;
  }

  d->handle = (uint32_t)handle;

  /* Track in per-fd list (Phase C) */
  if (cf && cf->created_dumb_count < MAX_DUMB_BUFFERS) {
    cf->created_dumb_handles[cf->created_dumb_count++] = (int)d->handle;
  }

  return 0;
}

/* DRM_IOCTL_MODE_MAP_DUMB */
static long drm_ioctl_map_dumb(void *arg) {
  struct drm_mode_map_dumb *m = (struct drm_mode_map_dumb *)arg;
  spin_lock(&g_drm.dumb_lock);
  struct drm_dumb_buffer *d = drm_find_dumb((int)m->handle);
  if (!d) {
    spin_unlock(&g_drm.dumb_lock);
    return -EINVAL;
  }
  m->offset = (uint64_t)m->handle << 12;
  spin_unlock(&g_drm.dumb_lock);
  return 0;
}

/* DRM_IOCTL_MODE_DESTROY_DUMB */
static void drm_file_untrack_handle(struct drm_file *df, int handle,
                                    bool virgl) {
  if (!df)
    return;
  int *handles = virgl ? df->created_virgl_handles : df->created_dumb_handles;
  int *count = virgl ? &df->created_virgl_count : &df->created_dumb_count;
  for (int i = 0; i < *count; i++) {
    if (handles[i] != handle)
      continue;
    handles[i] = handles[--(*count)];
    return;
  }
}

static long drm_ioctl_destroy_dumb(void *arg, struct drm_file *df) {
  struct drm_mode_destroy_dumb *d = (struct drm_mode_destroy_dumb *)arg;
  spin_lock(&g_drm.dumb_lock);
  struct drm_dumb_buffer *buf = drm_find_dumb((int)d->handle);
  if (!buf) {
    spin_unlock(&g_drm.dumb_lock);
    return -EINVAL;
  }
  drm_file_untrack_handle(df, (int)d->handle, false);
  buf->refcount--;
  if (buf->refcount <= 0) {
    uint32_t rid = buf->virtio_res_id;
    void *vaddr = buf->kernel_vaddr;
    uint32_t npages = (uint32_t)((buf->size + PAGE_SIZE - 1) / PAGE_SIZE);
    __memset(buf, 0, sizeof(*buf));
    spin_unlock(&g_drm.dumb_lock);
    virtio_gpu_resource_unref(rid);
    bfc_free_page_data(vaddr, npages);
    return 0;
  }
  spin_unlock(&g_drm.dumb_lock);
  return 0;
}

/* DRM_IOCTL_GEM_CLOSE
 * Called by Mesa after ADDFB2 to release the handle reference.
 * In this simplified model, same semantics as DESTROY_DUMB. */
static long drm_ioctl_gem_close(void *arg, struct drm_file *df) {
  struct drm_gem_close *c = (struct drm_gem_close *)arg;
  if (!c)
    return -EFAULT;

  /* virgl legacy (v1) resource: handle lives at/above VIRGL_HANDLE_BASE. */
  if ((uint32_t)c->handle >= VIRGL_HANDLE_BASE) {
    spin_lock(&g_drm.virgl_lock);
    struct drm_virgl_resource *r = drm_find_virgl_resource((uint32_t)c->handle);
    if (!r) {
      spin_unlock(&g_drm.virgl_lock);
      return -ENOENT;
    }
    drm_file_untrack_handle(df, (int)c->handle, true);
    r->refcount--;
    if (r->refcount > 0) {
      spin_unlock(&g_drm.virgl_lock);
      return 0;
    }
    uint32_t rid = r->res_handle;
    void *vaddr = r->kernel_vaddr;
    uint64_t sz = r->size;
    free_virgl_handle((uint32_t)c->handle); /* zeroes the slot */
    spin_unlock(&g_drm.virgl_lock);
    /* Tear down host resource + free guest backing outside the spinlock
     * (these block/sleep on the host). */
    virtio_gpu_resource_unref(rid);
    uint32_t npages = (sz + PAGE_SIZE - 1) / PAGE_SIZE;
    bfc_free_page_data(vaddr, npages);
    return 0;
  }

  if (c->handle == 0 || c->handle > MAX_DUMB_BUFFERS)
    return -ENOENT;

  spin_lock(&g_drm.dumb_lock);
  struct drm_dumb_buffer *buf = drm_find_dumb((int)c->handle);
  if (!buf) {
    spin_unlock(&g_drm.dumb_lock);
    return -ENOENT;
  }
  drm_file_untrack_handle(df, (int)c->handle, false);
  buf->refcount--;
  if (buf->refcount <= 0) {
    uint32_t rid = buf->virtio_res_id;
    void *vaddr = buf->kernel_vaddr;
    uint32_t npages = (uint32_t)((buf->size + PAGE_SIZE - 1) / PAGE_SIZE);
    __memset(buf, 0, sizeof(*buf));
    spin_unlock(&g_drm.dumb_lock);
    virtio_gpu_resource_unref(rid);
    bfc_free_page_data(vaddr, npages);
    return 0;
  }
  spin_unlock(&g_drm.dumb_lock);
  return 0;
}

/* DRM_IOCTL_MODE_ADDFB */
static long drm_ioctl_addfb(void *arg, struct drm_file *cf) {
  struct drm_mode_fb_cmd *f = (struct drm_mode_fb_cmd *)arg;
  spin_lock(&g_drm.dumb_lock);
  struct drm_dumb_buffer *d = drm_find_dumb((int)f->handle);
  int dref = d ? 1 : 0;
  spin_unlock(&g_drm.dumb_lock);
  if (!dref)
    return -EINVAL;

  spin_lock(&g_drm.fb_lock);
  int fb_id = drm_alloc_fb_id();
  if (fb_id < 0) {
    spin_unlock(&g_drm.fb_lock);
    return -ENOMEM;
  }
  struct drm_framebuffer *fb = &g_drm.fbs[fb_id - 1];
  spin_unlock(&g_drm.fb_lock);

  fb->dumb_handle = (int)f->handle;
  fb->is_virgl = false;
  fb->width = f->width;
  fb->height = f->height;
  fb->pitch = f->pitch;
  fb->bpp = f->bpp;

  spin_lock(&g_drm.dumb_lock);
  d = drm_find_dumb((int)f->handle);
  if (d)
    d->refcount++;
  spin_unlock(&g_drm.dumb_lock);

  f->fb_id = (uint32_t)fb_id;

  /* Track in per-fd list (Phase C) */
  if (cf && cf->created_fb_count < MAX_FRAMEBUFFERS) {
    cf->created_fb_ids[cf->created_fb_count++] = (int)fb_id;
  }

  return 0;
}

/* Helper: bpp from DRM pixel format */
static int bpp_from_format(uint32_t pixel_format) {
  switch (pixel_format) {
  case DRM_FORMAT_XRGB8888:
  case DRM_FORMAT_ARGB8888:
  case DRM_FORMAT_XBGR8888:
  case DRM_FORMAT_ABGR8888:
    return 32;
  case DRM_FORMAT_RGB565:
    return 16;
  default:
    return 0;
  }
}

/* DRM_IOCTL_MODE_ADDFB2 */
static long drm_ioctl_addfb2(void *arg, struct drm_file *cf) {
  struct drm_mode_fb_cmd2 *c = (struct drm_mode_fb_cmd2 *)arg;
  if (!c)
    return -EFAULT;

  /* Validate pixel format */
  int bpp = bpp_from_format(c->pixel_format);
  if (bpp == 0)
    return -EINVAL;

  /* Validate flags (no modifiers currently) */
  if (c->flags != 0)
    return -EINVAL;

  bool is_virgl = c->handles[0] >= VIRGL_HANDLE_BASE;
  struct drm_dumb_buffer *d = NULL;
  if (is_virgl) {
    spin_lock(&g_drm.virgl_lock);
    struct drm_virgl_resource *r = drm_find_virgl_resource(c->handles[0]);
    if (r)
      r->refcount++;
    spin_unlock(&g_drm.virgl_lock);
    if (!r)
      return -ENOENT;
  } else {
    spin_lock(&g_drm.dumb_lock);
    d = drm_find_dumb((int)c->handles[0]);
    if (d)
      d->refcount++;
    spin_unlock(&g_drm.dumb_lock);
    if (!d)
      return -ENOENT;
  }

  /* Allocate fb_id (shared with ADDFB) */
  spin_lock(&g_drm.fb_lock);
  int fb_id = drm_alloc_fb_id();
  if (fb_id < 0) {
    spin_unlock(&g_drm.fb_lock);
    struct drm_gem_close close = {.handle = c->handles[0]};
    drm_ioctl_gem_close(&close, NULL);
    return -ENOMEM;
  }
  struct drm_framebuffer *fb = &g_drm.fbs[fb_id - 1];
  spin_unlock(&g_drm.fb_lock);

  fb->dumb_handle = (int)c->handles[0];
  fb->is_virgl = is_virgl;
  fb->width = c->width;
  fb->height = c->height;
  fb->pitch = c->pitches[0];
  fb->bpp = (uint32_t)bpp;

  c->fb_id = (uint32_t)fb_id;

  /* Track in per-fd list (Phase C) */
  {
    if (cf && cf->created_fb_count < MAX_FRAMEBUFFERS) {
      cf->created_fb_ids[cf->created_fb_count++] = (int)fb_id;
    }
  }

  return 0;
}

/* DRM_IOCTL_MODE_GETFB (old-style) */
static long drm_ioctl_getfb(void *arg) {
  struct drm_mode_fb_cmd *f = (struct drm_mode_fb_cmd *)arg;
  if (!f)
    return -EFAULT;

  struct drm_framebuffer *fb = drm_find_fb((int)f->fb_id);
  if (!fb)
    return -ENOENT;

  f->width = fb->width;
  f->height = fb->height;
  f->pitch = fb->pitch;
  f->bpp = fb->bpp;
  f->depth = 24;
  f->handle = (uint32_t)fb->dumb_handle;
  return 0;
}

/* DRM_IOCTL_MODE_RMFB */
static long drm_ioctl_rmfb(void *arg) {
  uint32_t fb_id = *(uint32_t *)arg;
  spin_lock(&g_drm.fb_lock);
  struct drm_framebuffer *fb = drm_find_fb((int)fb_id);
  if (!fb) {
    spin_unlock(&g_drm.fb_lock);
    return -EINVAL;
  }
  fb->refcount--;
  int dumb_handle = fb->dumb_handle;
  if (fb->refcount <= 0) {
    __memset(fb, 0, sizeof(*fb));
  }
  spin_unlock(&g_drm.fb_lock);

  struct drm_gem_close close = {.handle = (uint32_t)dumb_handle};
  drm_ioctl_gem_close(&close, NULL);
  return 0;
}

/* ===== Cursor overlay (Phase C) ===== */
static void drm_cursor_overlay(struct drm_dumb_buffer *target) {
  if (!g_drm_cursor.dirty || !g_drm_cursor.enabled)
    return;

  uint32_t *fb = (uint32_t *)target->kernel_vaddr;
  int fb_w = (int)target->width;
  int fb_h = (int)target->height;

  int sx = (g_drm_cursor.x - g_drm_cursor.hotspot_x);
  int sy = (g_drm_cursor.y - g_drm_cursor.hotspot_y);

  for (int cy = 0; cy < CURSOR_HEIGHT; cy++) {
    for (int cx = 0; cx < CURSOR_WIDTH; cx++) {
      int fx = sx + cx, fy = sy + cy;
      if (fx < 0 || fx >= fb_w || fy < 0 || fy >= fb_h)
        continue;

      uint32_t cpixel = g_drm_cursor.buffer[cy * CURSOR_WIDTH + cx];
      uint8_t a = (cpixel >> 24) & 0xFF;
      if (a == 0)
        continue; /* fully transparent */
      if (a == 255) {
        fb[fy * fb_w + fx] = cpixel; /* opaque: replace */
      } else {
        /* alpha blend */
        uint32_t *dst = &fb[fy * fb_w + fx];
        uint32_t bg = *dst;
        uint8_t r = ((cpixel >> 16) & 0xFF) * a / 255 +
                    ((bg >> 16) & 0xFF) * (255 - a) / 255;
        uint8_t g = ((cpixel >> 8) & 0xFF) * a / 255 +
                    ((bg >> 8) & 0xFF) * (255 - a) / 255;
        uint8_t b = (cpixel & 0xFF) * a / 255 + (bg & 0xFF) * (255 - a) / 255;
        *dst = (0xFF << 24) | (r << 16) | (g << 8) | b;
      }
    }
  }

  g_drm_cursor.dirty = false;
}

/* DRM_IOCTL_MODE_CURSOR2 */
static long drm_ioctl_cursor2(void *arg) {
  struct drm_mode_cursor2 *c = (struct drm_mode_cursor2 *)arg;
  if (!c)
    return -EFAULT;
  if (c->crtc_id != DRM_CRTC_ID)
    return -EINVAL;

  switch (c->flags & DRM_MODE_CURSOR_FLAGS) {
  case DRM_MODE_CURSOR_BO: {
    if (c->handle == 0) {
      g_drm_cursor.enabled = false;
      g_drm_cursor.dirty = true;
      return 0;
    }
    /* Set cursor bitmap: c->handle is a dumb buffer handle containing cursor
     * image data */
    spin_lock(&g_drm.dumb_lock);
    struct drm_dumb_buffer *d = drm_find_dumb((int)c->handle);
    if (!d || d->size < CURSOR_SIZE) {
      spin_unlock(&g_drm.dumb_lock);
      return -EINVAL;
    }
    __memcpy(g_drm_cursor.buffer, d->kernel_vaddr, CURSOR_SIZE);
    spin_unlock(&g_drm.dumb_lock);
    g_drm_cursor.hotspot_x = (int16_t)c->hot_x;
    g_drm_cursor.hotspot_y = (int16_t)c->hot_y;
    g_drm_cursor.enabled = true;
    g_drm_cursor.dirty = true;
    return 0;
  }
  case DRM_MODE_CURSOR_MOVE:
    g_drm_cursor.x = (int16_t)c->x;
    g_drm_cursor.y = (int16_t)c->y;
    g_drm_cursor.dirty = true;
    return 0;
  default:
    return -EINVAL;
  }
}

/* Legacy cursor ioctl has the same fields as CURSOR2 except for hotspots. */
static long drm_ioctl_cursor(void *arg) {
  struct drm_mode_cursor *c = (struct drm_mode_cursor *)arg;
  if (!c)
    return -EFAULT;

  struct drm_mode_cursor2 c2 = {
      .flags = c->flags,
      .crtc_id = c->crtc_id,
      .x = c->x,
      .y = c->y,
      .width = c->width,
      .height = c->height,
      .handle = c->handle,
      .hot_x = 0,
      .hot_y = 0,
  };
  return drm_ioctl_cursor2(&c2);
}

/* DRM_IOCTL_MODE_PAGE_FLIP */
static long drm_ioctl_page_flip(void *arg) {
  struct drm_mode_crtc_page_flip *p = (struct drm_mode_crtc_page_flip *)arg;
  if (p->crtc_id != DRM_CRTC_ID)
    return -EINVAL;
  struct drm_framebuffer *fb = drm_find_fb((int)p->fb_id);
  if (!fb)
    return -EINVAL;
  struct drm_dumb_buffer *d = drm_find_dumb(fb->dumb_handle);
  uint32_t resource_id = d ? d->virtio_res_id : 0;
  if (fb->is_virgl) {
    struct drm_virgl_resource *r =
        drm_find_virgl_resource((uint32_t)fb->dumb_handle);
    resource_id = r ? r->res_handle : 0;
  }
  if (!resource_id)
    return -EINVAL;

  if (d) {
    drm_cursor_overlay(d);
    virtio_gpu_transfer_2d(resource_id, 0, 0, d->width, d->height, 0);
  }
  virtio_gpu_set_scanout(0, resource_id, 0, 0, fb->width, fb->height);
  virtio_gpu_flush(resource_id, 0, 0, fb->width, fb->height);

  g_drm.current_fb_id = p->fb_id;

  bool signal_event = false;
  if (p->flags & DRM_MODE_PAGE_FLIP_EVENT) {
    spin_lock(&g_drm.event_lock);
    g_drm.event_pending = true;
    g_drm.event_sequence++;
    g_drm.event_user_data = p->user_data;
    spin_unlock(&g_drm.event_lock);
    signal_event = true;
  }
  /* Wake poll waiters outside event_lock: __wake_up takes event_wq->lock,
   * and keeping the order (event_lock → wq->lock) one-directional avoids
   * any cross with drm_poll's event_lock-only read. */
  if (signal_event)
    __wake_up(&g_drm.event_wq, POLLIN);
  if (signal_event && drm_page_flip_log_count < 3) {
    drm_page_flip_log_count++;
    printk(LOG_INFO, "drm: page flip #%u queued fb=%u resource=%u\n",
           drm_page_flip_log_count, p->fb_id, resource_id);
  }
  return 0;
}

/* DRM_IOCTL_MODE_DIRTYFB */
static long drm_ioctl_dirtyfb(void *arg) {
  struct drm_mode_fb_dirty_cmd *c = (struct drm_mode_fb_dirty_cmd *)arg;
  struct drm_framebuffer *fb = drm_find_fb((int)c->fb_id);
  if (!fb)
    return -EINVAL;
  struct drm_dumb_buffer *d = drm_find_dumb(fb->dumb_handle);
  uint32_t resource_id = d ? d->virtio_res_id : 0;
  if (fb->is_virgl) {
    struct drm_virgl_resource *r =
        drm_find_virgl_resource((uint32_t)fb->dumb_handle);
    resource_id = r ? r->res_handle : 0;
  }
  if (!resource_id)
    return -EINVAL;
  if (d) {
    drm_cursor_overlay(d);
    virtio_gpu_transfer_2d(resource_id, 0, 0, d->width, d->height, 0);
  }
  virtio_gpu_flush(resource_id, 0, 0, fb->width, fb->height);
  return 0;
}

/* Main DRM ioctl dispatcher */
static long drm_ioctl_file(xtask *proc, struct file *file, uint32_t cmd,
                           void *arg) {
  (void)proc;
  struct drm_file *df = file ? (struct drm_file *)file->private_data : NULL;
  if (!df || !df->used)
    return -EBADF;
  printk(LOG_DEBUG, "drm_ioctl: cmd=0x%x initialized=%d\n", cmd,
         g_drm.initialized);
  if (!g_drm.initialized)
    return -ENODEV;
  switch (cmd) {
  case DRM_IOCTL_MODE_SETCRTC:
  case DRM_IOCTL_MODE_PAGE_FLIP:
  case DRM_IOCTL_MODE_CURSOR:
  case DRM_IOCTL_MODE_CURSOR2:
  case DRM_IOCTL_MODE_DIRTYFB:
  case DRM_IOCTL_MODE_SETPROPERTY:
  case DRM_IOCTL_MODE_OBJ_SETPROPERTY:
    if (!df->is_master)
      return -EACCES;
    break;
  default:
    break;
  }
  switch (cmd) {
  case DRM_IOCTL_VERSION:
    return drm_ioctl_version(arg);
  case DRM_IOCTL_VIRTGPU_GETPARAM:
    return drm_ioctl_virtgpu_getparam(arg);
  case DRM_IOCTL_VIRTGPU_GET_CAPS:
    return drm_ioctl_virtgpu_get_caps(arg);
  case DRM_IOCTL_VIRTGPU_CONTEXT_INIT:
    return drm_ioctl_virtgpu_context_init(arg, df);
  case DRM_IOCTL_VIRTGPU_RESOURCE_CREATE:
    return drm_ioctl_virtgpu_resource_create(arg, df);
  case DRM_IOCTL_VIRTGPU_RESOURCE_INFO:
    return drm_ioctl_virtgpu_resource_info(arg);
  case DRM_IOCTL_VIRTGPU_MAP:
    return drm_ioctl_virtgpu_map(arg, df);
  case DRM_IOCTL_VIRTGPU_EXECBUFFER:
    return drm_ioctl_virtgpu_execbuffer(arg, df);
  case DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST:
    return drm_ioctl_virtgpu_transfer_to_host(arg);
  case DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST:
    return drm_ioctl_virtgpu_transfer_from_host(arg);
  case DRM_IOCTL_VIRTGPU_WAIT:
    return drm_ioctl_virtgpu_3d_wait(arg, df);
  case DRM_IOCTL_GET_CAP:
    return drm_ioctl_get_cap(arg);
  case DRM_IOCTL_SET_CLIENT_CAP:
    return drm_ioctl_set_client_cap(arg);
  case DRM_IOCTL_SET_MASTER:
    return drm_ioctl_set_master(df);
  case DRM_IOCTL_DROP_MASTER:
    return drm_ioctl_drop_master(df);
  case DRM_IOCTL_MODE_GETRESOURCES:
    return drm_ioctl_getresources(arg);
  case DRM_IOCTL_MODE_GETCRTC:
    return drm_ioctl_getcrtc(arg);
  case DRM_IOCTL_MODE_SETCRTC:
    return drm_ioctl_setcrtc(arg);
  case DRM_IOCTL_MODE_GETCONNECTOR:
    return drm_ioctl_getconnector(arg);
  case DRM_IOCTL_MODE_GETENCODER:
    return drm_ioctl_getencoder(arg);
  case DRM_IOCTL_MODE_GETPLANERESOURCES:
    return drm_ioctl_getplaneres(arg);
  case DRM_IOCTL_MODE_GETPLANE:
    return drm_ioctl_getplane(arg);
  case DRM_IOCTL_MODE_CREATE_DUMB:
    return drm_ioctl_create_dumb(arg, df);
  case DRM_IOCTL_MODE_MAP_DUMB:
    return drm_ioctl_map_dumb(arg);
  case DRM_IOCTL_MODE_DESTROY_DUMB:
    return drm_ioctl_destroy_dumb(arg, df);
  case DRM_IOCTL_MODE_ADDFB:
    return drm_ioctl_addfb(arg, df);
  case DRM_IOCTL_MODE_ADDFB2:
    return drm_ioctl_addfb2(arg, df);
  case DRM_IOCTL_MODE_RMFB:
    return drm_ioctl_rmfb(arg);
  case DRM_IOCTL_MODE_PAGE_FLIP:
    return drm_ioctl_page_flip(arg);
  case DRM_IOCTL_MODE_DIRTYFB:
    return drm_ioctl_dirtyfb(arg);
  case DRM_IOCTL_MODE_CURSOR:
    return drm_ioctl_cursor(arg);
  case DRM_IOCTL_MODE_CURSOR2:
    return drm_ioctl_cursor2(arg);
  case DRM_IOCTL_MODE_GETFB:
    return drm_ioctl_getfb(arg);
  case DRM_IOCTL_GET_MAGIC:
    return drm_ioctl_get_magic(arg, df);
  case DRM_IOCTL_AUTH_MAGIC:
    return drm_ioctl_auth_magic(arg, df);
  case DRM_IOCTL_GEM_CLOSE:
    return drm_ioctl_gem_close(arg, df);
  case DRM_IOCTL_MODE_GETPROPERTY:
    return drm_ioctl_getproperty(arg);
  case DRM_IOCTL_MODE_SETPROPERTY:
    return drm_ioctl_setproperty(arg);
  case DRM_IOCTL_MODE_OBJ_SETPROPERTY:
    return drm_ioctl_obj_setproperty(arg);
  case DRM_IOCTL_MODE_GETPROPBLOB:
    return drm_ioctl_getpropblob(arg);
  case DRM_IOCTL_MODE_OBJ_GETPROPERTIES:
    return drm_ioctl_obj_getproperties(arg);
  case DRM_IOCTL_MODE_CREATE_LEASE:
    /* Empty leases are optional. wlroots falls back to reopening the node. */
    return -EOPNOTSUPP;
  case DRM_IOCTL_PRIME_HANDLE_TO_FD:
    return drm_ioctl_prime_handle_to_fd(arg, proc);
  case DRM_IOCTL_PRIME_FD_TO_HANDLE:
    return drm_ioctl_prime_fd_to_handle(arg, proc, df);
  default:
    printk(LOG_WARN, "drm_ioctl: unknown cmd 0x%x\n", cmd);
    return -ENOSYS;
  }
}

/* ctx_id pool: ctx_id 0 is reserved ("no context"), ids 1..MAX_CTX_IDS. */
static uint32_t alloc_ctx_id(void) {
  spin_lock(&g_drm.ctx_id_lock);
  for (uint32_t i = 0; i < MAX_CTX_IDS; i++) {
    if (!(g_drm.ctx_id_bitmap[i / 32] & (1u << (i % 32)))) {
      g_drm.ctx_id_bitmap[i / 32] |= (1u << (i % 32));
      spin_unlock(&g_drm.ctx_id_lock);
      return i + 1;
    }
  }
  spin_unlock(&g_drm.ctx_id_lock);
  return 0;
}

static void free_ctx_id(uint32_t id) {
  if (id == 0 || id > MAX_CTX_IDS)
    return;
  spin_lock(&g_drm.ctx_id_lock);
  g_drm.ctx_id_bitmap[(id - 1) / 32] &= ~(1u << ((id - 1) % 32));
  spin_unlock(&g_drm.ctx_id_lock);
}

/* blob handle: monotonic 1-based; slot reuse keyed by bo_handle. */
/* Virgl v1 handles mirror table indices. Probe from the last allocation so
 * GEM_CLOSE slots are reusable instead of permanently exhausting the pool. */
static uint32_t alloc_virgl_handle(void) {
  spin_lock(&g_drm.virgl_lock);
  uint32_t start = g_drm.next_virgl_handle - VIRGL_HANDLE_BASE;
  for (uint32_t i = 0; i < MAX_VIRGL_RESOURCES; i++) {
    uint32_t index = (start + i) % MAX_VIRGL_RESOURCES;
    struct drm_virgl_resource *r = &g_drm.virgl_res[index];
    if (r->bo_handle != 0)
      continue;

    uint32_t h = VIRGL_HANDLE_BASE + index;
    r->bo_handle = h; /* reserve the slot until resource creation completes */
    g_drm.next_virgl_handle =
        VIRGL_HANDLE_BASE + ((index + 1) % MAX_VIRGL_RESOURCES);
    spin_unlock(&g_drm.virgl_lock);
    return h;
  }
  spin_unlock(&g_drm.virgl_lock);
  return 0;
}

static struct drm_virgl_resource *drm_find_virgl_resource(uint32_t handle) {
  if (handle < VIRGL_HANDLE_BASE ||
      handle >= VIRGL_HANDLE_BASE + MAX_VIRGL_RESOURCES)
    return NULL;
  struct drm_virgl_resource *r = &g_drm.virgl_res[handle - VIRGL_HANDLE_BASE];
  return (r->bo_handle == handle) ? r : NULL;
}

static void free_virgl_handle(uint32_t handle) {
  if (handle < VIRGL_HANDLE_BASE ||
      handle >= VIRGL_HANDLE_BASE + MAX_VIRGL_RESOURCES)
    return;
  struct drm_virgl_resource *r = &g_drm.virgl_res[handle - VIRGL_HANDLE_BASE];
  if (r->bo_handle != handle)
    return;
  __memset(r, 0, sizeof(*r));
}

static bool drm_bo_get(uint32_t handle, bool is_virgl) {
  if (is_virgl) {
    spin_lock(&g_drm.virgl_lock);
    struct drm_virgl_resource *r = drm_find_virgl_resource(handle);
    if (r)
      r->refcount++;
    spin_unlock(&g_drm.virgl_lock);
    return r != NULL;
  }

  spin_lock(&g_drm.dumb_lock);
  struct drm_dumb_buffer *d = drm_find_dumb((int)handle);
  if (d)
    d->refcount++;
  spin_unlock(&g_drm.dumb_lock);
  return d != NULL;
}

void drm_prime_object_put(struct drm_prime_object *object) {
  if (!object)
    return;
  struct drm_gem_close close = {.handle = object->handle};
  drm_ioctl_gem_close(&close, NULL);
  kfree(object);
}

static bool drm_file_has_handle(struct drm_file *df, int handle, bool virgl) {
  int *handles = virgl ? df->created_virgl_handles : df->created_dumb_handles;
  int count = virgl ? df->created_virgl_count : df->created_dumb_count;
  for (int i = 0; i < count; i++) {
    if (handles[i] == handle)
      return true;
  }
  return false;
}

static long drm_ioctl_prime_handle_to_fd(void *arg, xtask *proc) {
  struct drm_prime_handle *prime = (struct drm_prime_handle *)arg;
  if (!prime || !proc)
    return -EFAULT;
  if (prime->flags & ~(DRM_CLOEXEC | DRM_RDWR))
    return -EINVAL;

  bool is_virgl = prime->handle >= VIRGL_HANDLE_BASE;
  if (!drm_bo_get(prime->handle, is_virgl))
    return -ENOENT;

  struct drm_prime_object *object = kmalloc(sizeof(*object));
  if (!object) {
    struct drm_gem_close close = {.handle = prime->handle};
    drm_ioctl_gem_close(&close, NULL);
    return -ENOMEM;
  }
  object->handle = prime->handle;
  object->is_virgl = is_virgl;

  int fd =
      bsd_drm_prime_fd_install(proc, object, (prime->flags & DRM_CLOEXEC) != 0);
  if (fd < 0) {
    drm_prime_object_put(object);
    return fd;
  }
  prime->fd = fd;
  return 0;
}

static long drm_ioctl_prime_fd_to_handle(void *arg, xtask *proc,
                                         struct drm_file *df) {
  struct drm_prime_handle *prime = (struct drm_prime_handle *)arg;
  if (!prime || !proc || !df)
    return -EFAULT;

  struct file *file = bsd_drm_prime_fd_get(proc, prime->fd);
  if (!file)
    return -EBADF;
  struct drm_prime_object *object = file->drm_prime;
  uint32_t handle = object->handle;
  bool is_virgl = object->is_virgl;

  if (!drm_file_has_handle(df, (int)handle, is_virgl)) {
    int *count = is_virgl ? &df->created_virgl_count : &df->created_dumb_count;
    int limit = is_virgl ? MAX_VIRGL_RESOURCES : MAX_DUMB_BUFFERS;
    int *handles =
        is_virgl ? df->created_virgl_handles : df->created_dumb_handles;
    if (*count >= limit) {
      file_put(file);
      return -ENOSPC;
    }
    if (!drm_bo_get(handle, is_virgl)) {
      file_put(file);
      return -ENOENT;
    }
    handles[(*count)++] = (int)handle;
  }

  prime->handle = handle;
  file_put(file);
  return 0;
}

/* True if capset_id was cached from the host. */
static bool virgl_capset_present(uint32_t capset_id) {
  bool found = false;
  spin_lock(&g_drm.capset_lock);
  for (uint32_t i = 0; i < g_drm.num_capsets; i++) {
    if (g_drm.capsets[i].id == capset_id) {
      found = true;
      break;
    }
  }
  spin_unlock(&g_drm.capset_lock);
  return found;
}

/* ===== DRM device ops ===== */
static int drm_open_file_common(xtask *proc, struct file *file,
                                bool is_render) {
  spin_lock(&g_drm_files_lock);
  for (int i = 0; i < MAX_DRM_FDS; i++) {
    if (!g_drm_files[i].used) {
      __memset(&g_drm_files[i], 0, sizeof(g_drm_files[i]));
      g_drm_files[i].used = true;
      g_drm_files[i].fd = -1;
      g_drm_files[i].proc = proc;
      g_drm_files[i].is_render = is_render;
      file->private_data = &g_drm_files[i];
      spin_unlock(&g_drm_files_lock);
      return 0;
    }
  }
  spin_unlock(&g_drm_files_lock);
  printk(LOG_ERROR, "drm: open-file table exhausted (%d slots)\n", MAX_DRM_FDS);
  return -ENFILE;
}

static int drm_open_file(xtask *proc, struct file *file) {
  return drm_open_file_common(proc, file, false);
}

/* Render node open: shares g_drm_files[] with card0 but marks is_render.
 * Render fds reject SET_MASTER/GET_MAGIC/AUTH_MAGIC. */
static int drm_render_open_file(xtask *proc, struct file *file) {
  return drm_open_file_common(proc, file, true);
}

/* Helper: release a framebuffer (refcount decrement + cleanup) */
static void drm_release_fb(int fb_id) {
  spin_lock(&g_drm.fb_lock);
  struct drm_framebuffer *fb = drm_find_fb(fb_id);
  if (!fb) {
    spin_unlock(&g_drm.fb_lock);
    return;
  }
  int dumb_handle = fb->dumb_handle;
  fb->refcount--;
  if (fb->refcount <= 0) {
    __memset(fb, 0, sizeof(*fb));
  }
  spin_unlock(&g_drm.fb_lock);

  if (dumb_handle > 0) {
    struct drm_gem_close close = {.handle = (uint32_t)dumb_handle};
    drm_ioctl_gem_close(&close, NULL);
  }
}

/* Drop the GEM-handle reference owned by a DRM file. */
static void drm_release_dumb(int handle) {
  struct drm_gem_close close = {.handle = (uint32_t)handle};
  drm_ioctl_gem_close(&close, NULL);
}

static int drm_close_file(xtask *proc, struct file *file) {
  struct drm_file *target = file ? (struct drm_file *)file->private_data : NULL;
  if (!target)
    return 0;
  spin_lock(&g_drm_files_lock);
  for (int i = 0; i < MAX_DRM_FDS; i++) {
    struct drm_file *f = &g_drm_files[i];
    if (!f->used || f != target)
      continue;
    /* Release per-fd resources (Phase C) */
    for (int j = 0; j < f->created_fb_count; j++) {
      drm_release_fb(f->created_fb_ids[j]);
    }
    for (int j = 0; j < f->created_dumb_count; j++) {
      drm_release_dumb(f->created_dumb_handles[j]);
    }

    /* Release Venus context (plan1) */
    if (f->ctx_id != 0) {
      struct virtio_gpu_ctx_create destroy;
      struct virtio_gpu_ctrl_hdr_response resp;
      __memset(&destroy, 0, sizeof(destroy));
      __memset(&resp, 0, sizeof(resp));
      destroy.hdr.type = VIRTIO_GPU_CMD_CTX_DESTROY;
      destroy.hdr.ctx_id = f->ctx_id;
      virtio_gpu_send_cmd_3d(&g_virtio_gpu, &destroy, sizeof(destroy), &resp,
                             sizeof(resp));
      drm_virgl_forget_context(f->ctx_id);
      free_ctx_id(f->ctx_id);
      f->ctx_id = 0;
    }
    if (f->ring_fence_counters) {
      kfree(f->ring_fence_counters);
      f->ring_fence_counters = NULL;
    }

    /* Release virgl legacy (v1) resources: unref host resource + free guest
     * backing. Per-fd tracking guarantees each handle is torn down once. */
    for (int j = 0; j < f->created_virgl_count; j++) {
      uint32_t h = (uint32_t)f->created_virgl_handles[j];
      spin_lock(&g_drm.virgl_lock);
      struct drm_virgl_resource *r = drm_find_virgl_resource(h);
      if (!r) {
        spin_unlock(&g_drm.virgl_lock);
        continue;
      }
      r->refcount--;
      if (r->refcount > 0) {
        spin_unlock(&g_drm.virgl_lock);
        continue;
      }
      uint32_t rid = r->res_handle;
      void *vaddr = r->kernel_vaddr;
      uint64_t sz = r->size;
      free_virgl_handle(h);
      spin_unlock(&g_drm.virgl_lock);
      virtio_gpu_resource_unref(rid);
      uint32_t npages = (sz + PAGE_SIZE - 1) / PAGE_SIZE;
      bfc_free_page_data(vaddr, npages);
    }

    if (f->is_master) {
      g_drm.is_master = false;
      drm_master_cleanup();
    }
    __memset(f, 0, sizeof(*f));
    file->private_data = NULL;
    spin_unlock(&g_drm_files_lock);
    return 0;
  }
  spin_unlock(&g_drm_files_lock);
  return 0; /* ignore close on unknown fd */
}

/* forward declarations for ops callbacks defined further below */
static ssize_t drm_read(xtask *proc, int fd, void *buf, size_t count);

struct dev_ops drm_dev_ops = {
    .driver_pid = 0,
    .is_block = false,
    .open_file = drm_open_file,
    .close_file = drm_close_file,
    .ioctl_file = drm_ioctl_file,
    .mmap = drm_mmap_handler,
    .read = drm_read,
    .poll = drm_poll,
};

/* Render node ops: kernel device, shares ioctl/mmap/close with card0,
 * rejects read/poll (no vblank events on render nodes). */
static struct dev_ops drm_render_ops = {
    .driver_pid = 0,
    .is_block = false,
    .open_file = drm_render_open_file,
    .close_file = drm_close_file,
    .ioctl_file = drm_ioctl_file,
    .mmap = drm_mmap_handler,
    .read = NULL,
    .poll = NULL,
};

/* DRM PCI 设备访问 (设计 C1) */
static struct pci_device *drm_pci_dev(void) { return g_virtio_gpu.vpci.pdev; }

/* sysfs show 回调 (priv=NULL, 读内核全局) */
static ssize_t drm_show_vendor(char *buf, size_t len, void *priv) {
  (void)priv;
  struct pci_device *pdev = drm_pci_dev();
  if (!pdev)
    return snprintf(buf, len, "0x0000\n");
  return snprintf(buf, len, "0x%04X\n", pdev->vendor_id);
}
static ssize_t drm_show_device(char *buf, size_t len, void *priv) {
  (void)priv;
  struct pci_device *pdev = drm_pci_dev();
  if (!pdev)
    return snprintf(buf, len, "0x0000\n");
  return snprintf(buf, len, "0x%04X\n", pdev->device_id);
}
static ssize_t drm_show_class(char *buf, size_t len, void *priv) {
  (void)priv;
  struct pci_device *pdev = drm_pci_dev();
  if (!pdev)
    return snprintf(buf, len, "0x000000\n");
  return snprintf(buf, len, "0x%06X\n", (uint32_t)pdev->class_code << 8);
}
static ssize_t drm_show_subsystem_vendor(char *buf, size_t len, void *priv) {
  (void)priv;
  struct pci_device *pdev = drm_pci_dev();
  if (!pdev)
    return snprintf(buf, len, "0x0000\n");
  uint32_t ids = pci_read_config(pdev->bus, pdev->dev, pdev->func, 0x2c);
  return snprintf(buf, len, "0x%04X\n", ids & 0xffff);
}
static ssize_t drm_show_subsystem_device(char *buf, size_t len, void *priv) {
  (void)priv;
  struct pci_device *pdev = drm_pci_dev();
  if (!pdev)
    return snprintf(buf, len, "0x0000\n");
  uint32_t ids = pci_read_config(pdev->bus, pdev->dev, pdev->func, 0x2c);
  return snprintf(buf, len, "0x%04X\n", ids >> 16);
}
static ssize_t drm_show_pci_uevent(char *buf, size_t len, void *priv) {
  (void)priv;
  struct pci_device *pdev = drm_pci_dev();
  if (!pdev)
    return -ENODEV;
  return snprintf(buf, len, "PCI_SLOT_NAME=0000:%02x:%02x.%u\n", pdev->bus,
                  pdev->dev, pdev->func);
}
static ssize_t drm_show_driver(char *buf, size_t len, void *priv) {
  (void)priv;
  return snprintf(buf, len, "virtio_gpu\n");
}
static ssize_t drm_show_enabled(char *buf, size_t len, void *priv) {
  (void)priv;
  return snprintf(buf, len, "%d\n", g_drm.initialized ? 1 : 0);
}
static ssize_t drm_show_mode(char *buf, size_t len, void *priv) {
  (void)priv;
  return snprintf(buf, len, "%ux%u\n", g_drm.fb_width, g_drm.fb_height);
}
static ssize_t drm_show_connector_status(char *buf, size_t len, void *priv) {
  (void)priv;
  return snprintf(buf, len, "connected\n");
}
static ssize_t drm_show_num_scanouts(char *buf, size_t len, void *priv) {
  (void)priv;
  return snprintf(buf, len, "%u\n", g_virtio_gpu.config.num_scanouts);
}

static ssize_t drm_attr_dev_show(char *buf, size_t len, void *priv) {
  (void)priv;
  /* DRM_MAJOR=226, minor=0 */
  return snprintf(buf, len, "226:0\n");
}

static const struct sysfs_attr drm_attr_dev = {
    .name = "dev", .show = drm_attr_dev_show, .priv = NULL};

static const struct sysfs_attr drm_attr_vendor = {
    .name = "vendor", .show = drm_show_vendor, .priv = NULL};
static const struct sysfs_attr drm_attr_device = {
    .name = "device", .show = drm_show_device, .priv = NULL};
static const struct sysfs_attr drm_attr_class = {
    .name = "class", .show = drm_show_class, .priv = NULL};
static const struct sysfs_attr drm_attr_subsystem_vendor = {
    .name = "subsystem_vendor",
    .show = drm_show_subsystem_vendor,
    .priv = NULL};
static const struct sysfs_attr drm_attr_subsystem_device = {
    .name = "subsystem_device",
    .show = drm_show_subsystem_device,
    .priv = NULL};
static const struct sysfs_attr drm_attr_pci_uevent = {
    .name = "uevent", .show = drm_show_pci_uevent, .priv = NULL};
static const struct sysfs_attr drm_attr_driver = {
    .name = "driver", .show = drm_show_driver, .priv = NULL};
static const struct sysfs_attr drm_attr_enabled = {
    .name = "enabled", .show = drm_show_enabled, .priv = NULL};
static const struct sysfs_attr drm_attr_mode = {
    .name = "mode", .show = drm_show_mode, .priv = NULL};
static const struct sysfs_attr drm_attr_connector_status = {
    .name = "connector_status",
    .show = drm_show_connector_status,
    .priv = NULL};
static const struct sysfs_attr drm_attr_num_scanouts = {
    .name = "num_scanouts", .show = drm_show_num_scanouts, .priv = NULL};

void drm_dev_register(void) {
  int rc = devtmpfs_create("dri/card0", &drm_dev_ops, NULL);
  if (rc < 0) {
    printk(LOG_ERROR, "drm: failed to create /dev/dri/card0: %d\n", rc);
    return;
  }
  __strncpy(drm_dev_ops.subsystem, "drm", 7);
  __strncpy(drm_dev_ops.devtype, "card", 7);
  drm_dev_ops.minor = 0;

  int rc2 = devtmpfs_create("dri/renderD128", &drm_render_ops, NULL);
  if (rc2 < 0) {
    printk(LOG_ERROR, "drm: failed to create /dev/dri/renderD128: %d\n", rc2);
  } else {
    __strncpy(drm_render_ops.subsystem, "drm", 7);
    __strncpy(drm_render_ops.devtype, "renderD128", 7);
    drm_render_ops.minor = 128;
  }

  struct sysfs_node *cls = sysfs_class_dir("drm");
  struct sysfs_node *card0 = sysfs_create_dir(cls, "card0");
  if (card0) {
    sysfs_create_file(card0, "vendor", &drm_attr_vendor);
    sysfs_create_file(card0, "device", &drm_attr_device);
    sysfs_create_file(card0, "class", &drm_attr_class);
    sysfs_create_file(card0, "driver", &drm_attr_driver);
    sysfs_create_file(card0, "enabled", &drm_attr_enabled);
    sysfs_create_file(card0, "mode", &drm_attr_mode);
    sysfs_create_file(card0, "connector_status", &drm_attr_connector_status);
    sysfs_create_file(card0, "num_scanouts", &drm_attr_num_scanouts);
    sysfs_create_file(card0, "dev", &drm_attr_dev);
    drm_dev_ops.sysfs_dir = card0;
  }

  struct sysfs_node *rnode = sysfs_create_dir(cls, "renderD128");
  if (rnode) {
    sysfs_create_file(rnode, "vendor", &drm_attr_vendor);
    sysfs_create_file(rnode, "device", &drm_attr_device);
    sysfs_create_file(rnode, "class", &drm_attr_class);
    sysfs_create_file(rnode, "driver", &drm_attr_driver);
    sysfs_create_file(rnode, "dev", &drm_attr_dev);
    drm_render_ops.sysfs_dir = rnode;
  }
  struct sysfs_node *card_devchar =
      sysfs_devchar_register(226, 0, "dri/card0", "/sys/bus/virtio");
  if (!card_devchar)
    printk(LOG_ERROR, "drm: failed to register /sys/dev/char/226:0\n");
  struct sysfs_node *render_devchar = NULL;
  if (rc2 >= 0) {
    render_devchar =
        sysfs_devchar_register(226, 128, "dri/renderD128", "/sys/bus/virtio");
    if (!render_devchar)
      printk(LOG_ERROR, "drm: failed to register /sys/dev/char/226:128\n");
  }
  if (card_devchar && render_devchar) {
    sysfs_devchar_add_device_child(card_devchar, "drm", "card0");
    sysfs_devchar_add_device_child(card_devchar, "drm", "renderD128");
    sysfs_devchar_add_device_child(render_devchar, "drm", "card0");
    sysfs_devchar_add_device_child(render_devchar, "drm", "renderD128");
  }
  if (card_devchar) {
    sysfs_devchar_add_device_file(card_devchar, "vendor", &drm_attr_vendor);
    sysfs_devchar_add_device_file(card_devchar, "device", &drm_attr_device);
    sysfs_devchar_add_device_file(card_devchar, "class", &drm_attr_class);
    sysfs_devchar_add_device_file(card_devchar, "subsystem_vendor",
                                  &drm_attr_subsystem_vendor);
    sysfs_devchar_add_device_file(card_devchar, "subsystem_device",
                                  &drm_attr_subsystem_device);
    sysfs_devchar_add_device_file(card_devchar, "uevent", &drm_attr_pci_uevent);
  }
  if (render_devchar) {
    sysfs_devchar_add_device_file(render_devchar, "vendor", &drm_attr_vendor);
    sysfs_devchar_add_device_file(render_devchar, "device", &drm_attr_device);
    sysfs_devchar_add_device_file(render_devchar, "class", &drm_attr_class);
    sysfs_devchar_add_device_file(render_devchar, "subsystem_vendor",
                                  &drm_attr_subsystem_vendor);
    sysfs_devchar_add_device_file(render_devchar, "subsystem_device",
                                  &drm_attr_subsystem_device);
    sysfs_devchar_add_device_file(render_devchar, "uevent",
                                  &drm_attr_pci_uevent);
  }
  printk(LOG_INFO, "drm: registered /dev/dri/card0\n");
}

/* ===== DRM mmap handler =====
   mmap(fd, offset) where offset = handle << 12 (from MAP_DUMB/VIRTGPU_MAP).
   Map dumb or legacy virgl BO backing pages into user space. */
__attribute__((no_sanitize("kernel-address"))) uint64_t
drm_mmap_handler(xtask *proc, uint64_t size, uint64_t offset) {
  /* offset = handle << PAGE_SHIFT (from MODE_MAP_DUMB or VIRTGPU_MAP). */
  uint32_t handle = (uint32_t)(offset >> PAGE_SHIFT);

  uint64_t map_phys = 0;
  uint64_t map_size = 0;
  if (handle >= VIRGL_HANDLE_BASE) {
    spin_lock(&g_drm.virgl_lock);
    struct drm_virgl_resource *r = drm_find_virgl_resource(handle);
    if (r) {
      map_phys = r->guest_phys;
      map_size = r->size;
    }
    spin_unlock(&g_drm.virgl_lock);
  } else {
    spin_lock(&g_drm.dumb_lock);
    struct drm_dumb_buffer *d = drm_find_dumb((int)handle);
    if (d) {
      map_phys = d->guest_phys;
      map_size = d->size;
    }
    spin_unlock(&g_drm.dumb_lock);
  }

  /* sys_mmap rounds the requested length to whole pages.  The backing store
   * is allocated the same way, so permit the padding in its final page. */
  uint64_t mapped_capacity = ALIGN_UP(map_size, PAGE_SIZE);
  if (!map_phys || size == 0 || size > mapped_capacity) {
    printk(LOG_ERROR,
           "drm_mmap: invalid mapping for handle %u (offset=0x%llx, "
           "size=%llu, capacity=%llu)\n",
           handle, (unsigned long long)offset, (unsigned long long)size,
           (unsigned long long)mapped_capacity);
    return (uint64_t)-EINVAL;
  }

  size_t npages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
  uint64_t *pml4 =
      (__force uint64_t *)phys_to_virt((__force phys_addr_t)proc->mm->cr3);
  uint64_t vaddr = proc->mm->mmap_brk;
  uint64_t pte_flags = PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX;

  for (size_t i = 0; i < npages; i++) {
    uint64_t page_phys = map_phys + i * PAGE_SIZE;
    if (!map_user_page_direct(pml4, vaddr + i * PAGE_SIZE, page_phys,
                              pte_flags)) {
      for (size_t j = 0; j < i; j++)
        unmap_user_pages(pml4, vaddr + j * PAGE_SIZE,
                         vaddr + (j + 1) * PAGE_SIZE, 1);
      return (uint64_t)-ENOMEM;
    }
  }

  mmap_region *region = (mmap_region *)kmalloc(sizeof(mmap_region));
  if (!region) {
    for (size_t i = 0; i < npages; i++)
      unmap_user_pages(pml4, vaddr + i * PAGE_SIZE, vaddr + (i + 1) * PAGE_SIZE,
                       1);
    return (uint64_t)-ENOMEM;
  }
  region->vaddr = vaddr;
  region->size = npages * PAGE_SIZE;
  region->phys = map_phys;
  region->shm_obj = NULL;
  region->fd = -1; // DRM GEM is a physical mapping, not fd-backed
  region->offset = 0;
  region->flags = KMAP_PHYSICAL;
  region->inode = NULL;
  region->shm_private_src = NULL;
  region->next = NULL;
  vma_insert_sorted(proc->mm, region);
  proc->mm->mmap_brk = vaddr + npages * PAGE_SIZE;

  return vaddr;
}

/* ===== DRM poll handler =====
   Returns POLLIN when a page flip event is pending. */
__poll drm_poll(xtask *proc, int events) {
  (void)proc;
  (void)events;
  spin_lock(&g_drm.event_lock);
  __poll revents = g_drm.event_pending ? POLLIN : 0;
  spin_unlock(&g_drm.event_lock);
  return revents;
}

/* ===== DRM read handler (deliver page flip event) ===== */
static ssize_t drm_read(xtask *proc, int fd, void *buf, size_t count) {
  (void)proc;
  (void)fd;
  spin_lock(&g_drm.event_lock);
  if (!g_drm.event_pending) {
    spin_unlock(&g_drm.event_lock);
    /* A level-triggered epoll entry can be stale after userspace drains the
     * preceding flip. Treat that read as an empty batch, not a fatal error. */
    return 0;
  }
  if (count < sizeof(struct drm_event_vblank)) {
    spin_unlock(&g_drm.event_lock);
    return -EINVAL;
  }
  struct drm_event_vblank ev;
  __memset(&ev, 0, sizeof(ev));
  ev.base.type = DRM_EVENT_FLIP_COMPLETE;
  ev.base.length = sizeof(ev);
  ev.user_data = g_drm.event_user_data;
  ev.sequence = g_drm.event_sequence;
  uint64_t now = sched_clock();
  ev.tv_sec = (uint32_t)(now / 1000000000ULL);
  ev.tv_usec = (uint32_t)((now % 1000000000ULL) / 1000ULL);
  ev.crtc_id = DRM_CRTC_ID;
  g_drm.event_pending = false;
  spin_unlock(&g_drm.event_lock);

  if (drm_flip_event_log_count < 3) {
    drm_flip_event_log_count++;
    printk(LOG_INFO, "drm: flip-complete event #%u delivered seq=%u\n",
           drm_flip_event_log_count, ev.sequence);
  }

  size_t cr = copy_to_user(buf, &ev, sizeof(ev));
  if (cr != 0)
    return -EFAULT;
  return (ssize_t)sizeof(ev);
}

/* Pre-query all capsets via GET_CAPSET_INFO + GET_CAPSET and cache them in
 * g_drm.capsets[]. The bitmask surfaced by GETPARAM(SUPPORTED_CAPSET_IDs) and
 * the capsets served by GET_CAPS reflect exactly what the host advertises —
 * nothing is synthesized. The Venus path is retired; only host-provided virgl
 * capsets (1/2) drive the GL winsys. */
static void drm_query_capsets(struct virtio_gpu_device *vgpu) {
  g_drm.capset_lock = SPINLOCK_INIT;
  uint32_t n = vgpu->config.num_capsets;
  if (n > MAX_CAPSETS)
    n = MAX_CAPSETS;
  g_drm.num_capsets = 0;

  for (uint32_t i = 0; i < n; i++) {
    struct virtio_gpu_get_capset_info {
      struct virtio_gpu_ctrl_hdr hdr;
      uint32_t capset_index;
      uint32_t padding;
    } info_cmd;
    __memset(&info_cmd, 0, sizeof(info_cmd));
    info_cmd.hdr.type = VIRTIO_GPU_CMD_GET_CAPSET_INFO;
    info_cmd.capset_index = i;

    struct virtio_gpu_resp_capset_info info_resp;
    __memset(&info_resp, 0, sizeof(info_resp));
    if (virtio_gpu_send_cmd_3d(vgpu, &info_cmd, sizeof(info_cmd), &info_resp,
                               sizeof(info_resp)) < 0)
      continue;
    if (info_resp.hdr.type != VIRTIO_GPU_RESP_OK_CAPSET_INFO)
      continue;

    uint32_t csz = info_resp.capset_max_size;
    size_t resp_size = sizeof(struct virtio_gpu_ctrl_hdr) + csz;
    struct virtio_gpu_resp_capset *cap_resp = kmalloc(resp_size);
    if (!cap_resp)
      continue;
    __memset(cap_resp, 0, resp_size);

    struct virtio_gpu_get_capset get_cmd;
    __memset(&get_cmd, 0, sizeof(get_cmd));
    get_cmd.hdr.type = VIRTIO_GPU_CMD_GET_CAPSET;
    get_cmd.capset_id = info_resp.capset_id;
    get_cmd.capset_version = info_resp.capset_max_version;

    if (virtio_gpu_send_cmd_3d(vgpu, &get_cmd, sizeof(get_cmd), cap_resp,
                               resp_size) < 0 ||
        cap_resp->hdr.type != VIRTIO_GPU_RESP_OK_CAPSET) {
      kfree(cap_resp);
      continue;
    }

    void *cdata = kmalloc(csz);
    if (!cdata) {
      kfree(cap_resp);
      continue;
    }
    __memcpy(cdata, cap_resp->capset_data, csz);
    kfree(cap_resp);

    uint32_t slot = g_drm.num_capsets;
    g_drm.capsets[slot].id = info_resp.capset_id;
    g_drm.capsets[slot].ver = info_resp.capset_max_version;
    g_drm.capsets[slot].size = csz;
    g_drm.capsets[slot].data = cdata;
    g_drm.num_capsets++;
    printk(LOG_INFO, "drm: capset id=%u version=%u size=%u\n",
           info_resp.capset_id, info_resp.capset_max_version, csz);
  }

  printk(LOG_INFO, "drm: cached %u capsets\n", g_drm.num_capsets);
}

void virtio_gpu_init(void) {
  struct virtio_gpu_device *vgpu = &g_virtio_gpu;
  __memset(vgpu, 0, sizeof(*vgpu));
  vgpu->cmd_lock = SPINLOCK_INIT;
  init_wait_queue_head(&vgpu->cmd_wq);
  vgpu->pending_lock = SPINLOCK_INIT;
  vgpu->pending_list = NULL;

  /* Find PCI device */
  pci_device *pdev =
      pci_find_device_by_id(VIRTIO_PCI_VENDOR_ID, VIRTIO_PCI_DEVICE_ID);
  if (!pdev) {
    printk(LOG_ERROR, "virtio_gpu: PCI device not found\n");
    return;
  }
  printk(LOG_INFO, "virtio_gpu: found PCI device bus=%d dev=%d func=%d\n",
         pdev->bus, pdev->dev, pdev->func);

  /* Initialize transport */
  if (virtio_pci_init(&vgpu->vpci, pdev) < 0) {
    printk(LOG_ERROR, "virtio_gpu: virtio_pci_init failed\n");
    return;
  }

  /* Negotiate features: VERSION_1 + VIRGL(3D/context) + CONTEXT_INIT(multi-
   * ring). The blob feature (Venus resource model) is retired; virgl uses the
   * legacy v1 RESOURCE_CREATE path and does not need it. */
  uint64_t want = (1ULL << VIRTIO_F_VERSION_1) | (1ULL << VIRTIO_GPU_F_VIRGL) |
                  (1ULL << VIRTIO_GPU_F_CONTEXT_INIT);
  if (virtio_pci_negotiate_features(&vgpu->vpci, want) < 0) {
    printk(LOG_ERROR, "virtio_gpu: feature negotiation failed\n");
    return;
  }

  /* Allocate single MSI-X vector for ctrlq + config change */
  int nvectors = pci_enable_msix(pdev, 1);
  if (nvectors <= 0) {
    printk(LOG_ERROR, "virtio_gpu: pci_enable_msix failed\n");
    return;
  }
  vgpu->vpci.msix_vector = pdev->msix_vector_base;
  printk(LOG_INFO, "virtio_gpu: MSI-X vector %d\n", vgpu->vpci.msix_vector);

  /* Initialize ctrlq */
  if (virtio_gpu_init_ctrlq(vgpu) < 0) {
    printk(LOG_ERROR, "virtio_gpu: ctrlq init failed\n");
    return;
  }

  /* Wire up vring completion callback: vring_poll_used calls this for each
     completed descriptor, setting the per-command completed flag so each
     sleeping caller can independently detect its own response. */
  vgpu->ctrlq.callback = virtio_gpu_cmd_callback;

  /* Register ISR */
  irq_register(vgpu->vpci.msix_vector, virtio_gpu_isr);
  pci_msix_unmask_entry(pdev, 0);

  /* Set DRIVER_OK status (preserve FEATURES_OK negotiated earlier) */
  virtio_pci_write_status(
      &vgpu->vpci, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                       VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

  /* Read device config (num_scanouts) */
  virtio_pci_read_dev_cfg(&vgpu->vpci, 0, &vgpu->config, sizeof(vgpu->config));
  printk(LOG_INFO, "virtio_gpu: num_scanouts=%u num_capsets=%u\n",
         vgpu->config.num_scanouts, vgpu->config.num_capsets);

  /* Initialize DRM device state */
  __memset(&g_drm, 0, sizeof(g_drm));
  g_drm.initialized = true;
  g_drm.dumb_lock = SPINLOCK_INIT;
  g_drm.fb_lock = SPINLOCK_INIT;
  g_drm.event_lock = SPINLOCK_INIT;
  init_wait_queue_head(&g_drm.event_wq);
  g_drm.ctx_id_lock = SPINLOCK_INIT;
  g_drm.fence_lock = SPINLOCK_INIT;
  g_drm.virgl_lock = SPINLOCK_INIT;
  g_drm.next_virgl_handle = VIRGL_HANDLE_BASE;
  g_drm.next_dumb_handle = 1;
  g_drm.next_fb_id = 1;
  /* Query capsets after g_drm is zeroed/initialized. */
  drm_query_capsets(vgpu);

  /* Default display mode (runtime-overridable; see g_drm.fb_*).
     Change DRM_FB_WIDTH/HEIGHT to alter the default. */
  g_drm.fb_width = DRM_FB_WIDTH;
  g_drm.fb_height = DRM_FB_HEIGHT;
  g_drm.fb_bpp = DRM_FB_BPP;
  g_drm.fb_pitch = g_drm.fb_width * (g_drm.fb_bpp / 8);

  /* ===== Property infrastructure initialization (Phase C) ===== */
  /* Create properties */
  uint32_t p_src_x = drm_property_create_range("SRC_X", 0, 0xFFFFFFFF, true);
  uint32_t p_src_y = drm_property_create_range("SRC_Y", 0, 0xFFFFFFFF, true);
  uint32_t p_src_w = drm_property_create_range("SRC_W", 0, 0xFFFFFFFF, true);
  uint32_t p_src_h = drm_property_create_range("SRC_H", 0, 0xFFFFFFFF, true);
  uint32_t p_active = drm_property_create_range("ACTIVE", 0, 1, false);

  const uint64_t dpms_vals[4] = {0, 1, 2, 3};
  const char *dpms_names[4] = {"On", "Standby", "Suspend", "Off"};
  uint32_t p_dpms =
      drm_property_create_enum("DPMS", dpms_vals, dpms_names, 4, false);

  uint32_t p_edid = drm_property_create_blob("EDID", true);
  uint32_t p_in_formats = drm_property_create_blob("IN_FORMATS", true);
  uint32_t p_crtc_id =
      drm_property_create_object("CRTC_ID", DRM_MODE_OBJECT_CRTC, false);
  uint32_t p_fb_id =
      drm_property_create_object("FB_ID", DRM_MODE_OBJECT_FB, false);
  uint32_t p_mode_id = drm_property_create_blob("MODE_ID", false);
  const uint64_t plane_type_vals[3] = {
      DRM_PLANE_TYPE_OVERLAY, DRM_PLANE_TYPE_PRIMARY, DRM_PLANE_TYPE_CURSOR};
  const char *plane_type_names[3] = {"Overlay", "Primary", "Cursor"};
  uint32_t p_plane_type = drm_property_create_enum("type", plane_type_vals,
                                                   plane_type_names, 3, true);

  /* Generate IN_FORMATS blob */
  struct drm_format_modifier_blob {
    uint32_t version;
    uint32_t count_formats;
    uint32_t formats_offset;
    uint32_t count_modifiers;
    uint32_t modifiers_offset;
  } __attribute__((packed));

  uint32_t in_fmts[4] = {
      DRM_FORMAT_XRGB8888,
      DRM_FORMAT_ARGB8888,
      DRM_FORMAT_XBGR8888,
      DRM_FORMAT_ABGR8888,
  };

  struct drm_format_modifier {
    uint64_t offset;
    uint64_t width;
    uint64_t modifier;
  };

  struct drm_format_modifier in_mods[1];
  in_mods[0].offset = 0;
  in_mods[0].width = 4;
  in_mods[0].modifier = DRM_FORMAT_MOD_LINEAR;

  uint32_t in_fmts_blob_size =
      (uint32_t)(sizeof(struct drm_format_modifier_blob) +
                 4 * sizeof(uint32_t) + 1 * sizeof(struct drm_format_modifier));
  uint8_t *in_fmts_blob_data = (uint8_t *)kmalloc(in_fmts_blob_size);
  if (in_fmts_blob_data) {
    struct drm_format_modifier_blob *hdr =
        (struct drm_format_modifier_blob *)in_fmts_blob_data;
    __memset(hdr, 0, sizeof(*hdr));
    hdr->version = 1;
    hdr->count_formats = 4;
    hdr->formats_offset = sizeof(struct drm_format_modifier_blob);
    hdr->count_modifiers = 1;
    hdr->modifiers_offset = (uint32_t)(sizeof(struct drm_format_modifier_blob) +
                                       4 * sizeof(uint32_t));
    __memcpy(in_fmts_blob_data + sizeof(struct drm_format_modifier_blob),
             in_fmts, 4 * sizeof(uint32_t));
    __memcpy(in_fmts_blob_data + hdr->modifiers_offset, in_mods,
             sizeof(struct drm_format_modifier));
  }
  uint32_t in_fmts_blob_id =
      in_fmts_blob_data ? drm_blob_create(in_fmts_blob_data, in_fmts_blob_size)
                        : 0;
  if (in_fmts_blob_data)
    kfree(in_fmts_blob_data);

  /* Generate EDID blob */
  uint8_t edid_data[128];
  extern void drm_generate_edid(uint8_t * buf, size_t bufsz, uint32_t width,
                                uint32_t height);
  drm_generate_edid(edid_data, sizeof(edid_data), g_drm.fb_width,
                    g_drm.fb_height);
  uint32_t edid_blob_id = drm_blob_create(edid_data, 128);

  /* Bind properties to objects */
  /* Connector(2): DPMS + EDID */
  drm_property_add_to_object(DRM_MODE_OBJECT_CONNECTOR, DRM_CONNECTOR_ID,
                             p_dpms, DRM_MODE_DPMS_ON);
  drm_property_add_to_object(DRM_MODE_OBJECT_CONNECTOR, DRM_CONNECTOR_ID,
                             p_edid, edid_blob_id);

  /* Plane(4): type + IN_FORMATS + CRTC_ID + FB_ID + SRC_X/Y/W/H */
  drm_property_add_to_object(DRM_MODE_OBJECT_PLANE, DRM_PLANE_ID, p_plane_type,
                             DRM_PLANE_TYPE_PRIMARY);
  drm_property_add_to_object(DRM_MODE_OBJECT_PLANE, DRM_PLANE_ID, p_in_formats,
                             in_fmts_blob_id);
  drm_property_add_to_object(DRM_MODE_OBJECT_PLANE, DRM_PLANE_ID, p_crtc_id, 0);
  drm_property_add_to_object(DRM_MODE_OBJECT_PLANE, DRM_PLANE_ID, p_fb_id, 0);
  drm_property_add_to_object(DRM_MODE_OBJECT_PLANE, DRM_PLANE_ID, p_src_x, 0);
  drm_property_add_to_object(DRM_MODE_OBJECT_PLANE, DRM_PLANE_ID, p_src_y, 0);
  drm_property_add_to_object(DRM_MODE_OBJECT_PLANE, DRM_PLANE_ID, p_src_w, 0);
  drm_property_add_to_object(DRM_MODE_OBJECT_PLANE, DRM_PLANE_ID, p_src_h, 0);

  /* CRTC(1): ACTIVE + MODE_ID */
  drm_property_add_to_object(DRM_MODE_OBJECT_CRTC, DRM_CRTC_ID, p_active, 0);
  drm_property_add_to_object(DRM_MODE_OBJECT_CRTC, DRM_CRTC_ID, p_mode_id, 0);

  /* Register /dev/dri/card0 */
  drm_dev_register();

  printk(LOG_INFO, "virtio_gpu: init done\n");
}

dev_driver virtio_gpu_driver = {
    .name = "virtio_gpu",
    .pci_class = 0,
    .pci_vendor = VIRTIO_PCI_VENDOR_ID,
    .pci_device = VIRTIO_PCI_DEVICE_ID,
    .pci_subsystem_id = 0, /* subsystem_id cannot distinguish virtio devices */
    .init = virtio_gpu_init,
    .ops = NULL, /* ops set in Phase 3 (DRM/KMS) */
};

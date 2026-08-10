/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/driver/virtio_gpu.h"
#include "xos/perf.h"

#include <stdbool.h>
#include <stddef.h>

#include "arch/x64/apic.h"
#include "arch/x64/memlayout.h"
#include "arch/x64/paging.h"
#include "arch/x64/smp.h"
#include "arch/x64/trap.h"
#include "arch/x64/utils.h"
#include "kernel/bsd/devtmpfs.h"
#include "kernel/bsd/kfcntl.h" // IWYU pragma: keep
#include "kernel/bsd/sysfs.h"
#include "kernel/driver/bsd_types.h"
#include "kernel/driver/drm/drm_core.h"
#include "kernel/driver/drm/drm_fence.h"
#include "kernel/driver/drm_internal.h"
#include "kernel/driver/pci.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/mem/kasan.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/perf/event.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/workqueue.h"
#include "kernel/xcore/xtask.h"
#include "utils/macro.h"

#include <xos/errno.h>
#include <xos/page.h>

#include "drm/drm.h"
#include "drm/drm_fourcc.h"
#include "drm/drm_mode.h"
#include "drm/virtgpu_drm.h"

#define DRM_MAJOR 226
#define DRM_VBLANK_INTERVAL_NS (1000000000ULL / 60ULL)

struct virtio_gpu_backend {
  struct virtio_gpu_device vgpu;
  struct drm_device drm;
  spinlock files_lock;
  struct drm_file **files;
  int files_capacity;
  struct drm_core_device *core;
  struct workqueue *event_wq;
  struct work event_work;
  bool accepting_commands;
  bool hardware_live;
};

/* IRQ and timer hooks have no callback context. Only one virtio-gpu is
 * supported today, but its owned state lives in the PCI driver's private
 * allocation rather than DRM globals. */
static struct virtio_gpu_backend *virtio_gpu_backend;
static uint32_t drm_page_flip_log_count;
struct drm_gem_object;

#define DRM_CRTC_ID (virtio_gpu_backend->drm.crtc_id)
#define DRM_CONNECTOR_ID (virtio_gpu_backend->drm.connector_id)
#define DRM_ENCODER_ID (virtio_gpu_backend->drm.encoder_id)
#define DRM_PLANE_ID (virtio_gpu_backend->drm.plane_id)

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
    if (sc->waiter)
      wake_wq_target(sc->waiter);
  }
  (void)len;
}

/* Forward declarations */
static void virtio_gpu_isr(trapframe *tf);
static int virtio_gpu_send_cmd(struct virtio_gpu_device *vgpu, void *cmd_buf,
                               size_t cmd_len, void *resp_buf, size_t resp_len);
static int drm_dev_register(void);
static bool drm_dev_alloc(void);
static void virtio_gpu_backend_release(void *driver_private);
static void virtio_gpu_remove(pci_device *pdev);

/* plan2 forward declarations: these are defined later in the file but used by
 * the ISR (drm_fence_find/signal) and the EXECBUFFER ioctl (drm_file_current)
 * which precede their definitions. */
static struct drm_fence *drm_fence_find(uint32_t ctx_id, uint8_t ring_idx,
                                        uint64_t fence_id);
static void virtio_drm_fence_signal(struct drm_fence *fence, uint32_t ctx_id,
                                    uint8_t ring_idx, uint64_t fence_id);

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
    return -EINVAL;
  }
  printk(LOG_INFO, "virtio_gpu: ctrlq size=%u notify_off=%u\n", size,
         notify_off);

  /* Allocate and initialize the virtqueue */
  int rc = vring_create(&vgpu->ctrlq, VIRTIO_GPU_CTRLQ_INDEX, size, notify_off);
  if (rc < 0) {
    printk(LOG_ERROR, "virtio_gpu: vring_create failed: %d\n", rc);
    return rc;
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
  if (!virtio_gpu_backend ||
      !__atomic_load_n(&virtio_gpu_backend->hardware_live, __ATOMIC_ACQUIRE)) {
    lapic_eoi();
    return;
  }
  struct virtio_gpu_device *vgpu = &virtio_gpu_backend->vgpu;
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
        perf_trace_causal(XOS_PERF_TRACE_IO, XOS_PERF_IO_COMPLETE,
                          0xc0000000U | (uint32_t)p->hdr.fence_id);
        if (p->hdr.flags & VIRTIO_GPU_FLAG_FENCE) {
          struct drm_fence *f =
              drm_fence_find(p->hdr.ctx_id, p->hdr.ring_idx, p->hdr.fence_id);
          virtio_drm_fence_signal(f, p->hdr.ctx_id, p->hdr.ring_idx,
                                  p->hdr.fence_id);
          drm_fence_put(f); /* drop the in-flight submission reference */
        }
        if (p->waiter)
          wake_wq_target(p->waiter);
        if (p->waiter)
          perf_trace_causal(XOS_PERF_TRACE_IO, XOS_PERF_IO_WAKE,
                            0xc0000000U | (uint32_t)p->hdr.fence_id);
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
  if (!virtio_gpu_backend || vgpu != &virtio_gpu_backend->vgpu ||
      !__atomic_load_n(&virtio_gpu_backend->accepting_commands,
                       __ATOMIC_ACQUIRE) ||
      !__atomic_load_n(&virtio_gpu_backend->hardware_live, __ATOMIC_ACQUIRE))
    return -ENODEV;
  /* Per-command completion context: vring callback sets completed=true
     when the device processes this descriptor.  Each caller has its own
     ctx on the stack, so concurrent send_cmd invocations don't clobber
     each other's state. */
  struct virtgpu_sync_ctx cmd_ctx = {
      .tag = VIRTGPU_CTX_SYNC, .completed = false, .waiter = current_task};

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
    printk(LOG_ERROR, "virtio_gpu: vring_add_buf failed\n");
    return -1;
  }

  /* cmd_lock is also held by the completion callback.  Arm the task while
     holding both cmd_lock and its scheduler lock, so completion cannot race
     between the completion test and BLOCKED transition. */
  xtask *waiter = current_task;
  int wait_cpu = waiter->assigned_cpu;
  spin_lock(&cpu_locals[wait_cpu].scheduler_lock);
  waiter->state = BLOCKED;
  spin_unlock(&cpu_locals[wait_cpu].scheduler_lock);

  vring_kick(&vgpu->ctrlq);
  virtio_pci_notify(&vgpu->vpci, vgpu->ctrlq.notify_off);

  /* Release cmd_lock before sleeping; the ISR completes this exact context
     and wakes only its submitter. */
  spin_unlock_irqrestore(&vgpu->cmd_lock, irq_flags);

  for (;;) {
    schedule();
    if (cmd_ctx.completed)
      break;

    /* Signals may wake a blocked task without completing the command.  Take
       cmd_lock before re-arming so the ISR cannot publish completion between
       this check and the BLOCKED transition. */
    spin_lock_irqsave(&vgpu->cmd_lock, &irq_flags);
    if (!cmd_ctx.completed) {
      wait_cpu = waiter->assigned_cpu;
      spin_lock(&cpu_locals[wait_cpu].scheduler_lock);
      waiter->state = BLOCKED;
      spin_unlock(&cpu_locals[wait_cpu].scheduler_lock);
    }
    spin_unlock_irqrestore(&vgpu->cmd_lock, irq_flags);
  }

  return 0;
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
  if (!virtio_gpu_backend || vgpu != &virtio_gpu_backend->vgpu ||
      !__atomic_load_n(&virtio_gpu_backend->accepting_commands,
                       __ATOMIC_ACQUIRE) ||
      !__atomic_load_n(&virtio_gpu_backend->hardware_live, __ATOMIC_ACQUIRE))
    return -ENODEV;
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

  int rc = virtio_gpu_send_cmd(&virtio_gpu_backend->vgpu, &cmd, sizeof(cmd),
                               &resp, sizeof(resp));
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

  int rc = virtio_gpu_send_cmd(&virtio_gpu_backend->vgpu, &buf, sizeof(buf),
                               &resp, sizeof(resp));
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

  int rc = virtio_gpu_send_cmd(&virtio_gpu_backend->vgpu, &cmd, sizeof(cmd),
                               &resp, sizeof(resp));
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

  int rc = virtio_gpu_send_cmd(&virtio_gpu_backend->vgpu, &cmd, sizeof(cmd),
                               &resp, sizeof(resp));
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

  int rc = virtio_gpu_send_cmd(&virtio_gpu_backend->vgpu, &cmd, sizeof(cmd),
                               &resp, sizeof(resp));
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

  int rc = virtio_gpu_send_cmd(&virtio_gpu_backend->vgpu, &cmd, sizeof(cmd),
                               &resp, sizeof(resp));
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
  struct drm_dumb_buffer *d = &virtio_gpu_backend->drm.dumbs[handle - 1];
  return (d->handle == handle) ? d : NULL;
}

static struct drm_framebuffer *drm_find_fb(int fb_id) {
  if (fb_id <= 0 || fb_id > MAX_FRAMEBUFFERS)
    return NULL;
  struct drm_framebuffer *fb = &virtio_gpu_backend->drm.fbs[fb_id - 1];
  return (fb->fb_id == fb_id) ? fb : NULL;
}

static int drm_alloc_dumb_handle(void) {
  for (int i = 0; i < MAX_DUMB_BUFFERS; i++) {
    if (virtio_gpu_backend->drm.dumbs[i].handle == 0 &&
        virtio_gpu_backend->drm.dumbs[i].release_work.state == WORK_IDLE) {
      virtio_gpu_backend->drm.dumbs[i].handle =
          i + 1; /* handle = slot index + 1 */
      virtio_gpu_backend->drm.dumbs[i].refcount = 1;
      return virtio_gpu_backend->drm.dumbs[i].handle;
    }
  }
  return -1;
}

static struct page **drm_gem_contiguous_pages(uint64_t guest_phys,
                                              uint32_t page_count) {
  struct page **pages = kmalloc(sizeof(*pages) * page_count);
  if (!pages)
    return NULL;
  for (uint32_t i = 0; i < page_count; i++)
    pages[i] = &bfc_frames[PHY_TO_PAGE(guest_phys + i * PAGE_SIZE)];
  return pages;
}

static void drm_dumb_release_work(struct work *work) {
  struct drm_dumb_buffer *buf =
      LIST_ENTRY(work, struct drm_dumb_buffer, release_work);
  spin_lock(&virtio_gpu_backend->drm.dumb_lock);
  uint32_t rid = buf->virtio_res_id;
  void *vaddr = buf->kernel_vaddr;
  uint32_t npages = (uint32_t)ALIGN_UP(buf->size, PAGE_SIZE) / PAGE_SIZE;
  buf->refcount = 0;
  buf->gem = NULL;
  buf->guest_phys = 0;
  buf->kernel_vaddr = NULL;
  buf->width = 0;
  buf->height = 0;
  buf->pitch = 0;
  buf->size = 0;
  buf->virtio_res_id = 0;
  buf->handle = 0;
  spin_unlock(&virtio_gpu_backend->drm.dumb_lock);
  printk(LOG_INFO, "drm gem free dumb rid=%u vaddr=%p pages=%u\n", rid, vaddr,
         npages);
  virtio_gpu_resource_unref(rid);
  bfc_free_page_data(vaddr, npages);
}

static void drm_dumb_gem_release(struct drm_gem_object *object) {
  struct drm_dumb_buffer *buf = drm_gem_object_private(object);
  BUG_ON(!buf || buf->gem != object);
  if (virtio_gpu_backend->event_wq)
    BUG_ON(!queue_work(virtio_gpu_backend->event_wq, &buf->release_work));
  else
    drm_dumb_release_work(&buf->release_work);
}

static void drm_virgl_release_work(struct work *work) {
  struct drm_virgl_resource *resource =
      LIST_ENTRY(work, struct drm_virgl_resource, release_work);
  spin_lock(&virtio_gpu_backend->drm.virgl_lock);
  uint32_t rid = resource->res_handle;
  void *vaddr = resource->kernel_vaddr;
  uint32_t npages = (uint32_t)ALIGN_UP(resource->size, PAGE_SIZE) / PAGE_SIZE;
  resource->res_handle = 0;
  resource->guest_phys = 0;
  resource->kernel_vaddr = NULL;
  resource->size = 0;
  resource->refcount = 0;
  resource->gem = NULL;
  __memset(resource->ctx_attach_bitmap, 0, sizeof(resource->ctx_attach_bitmap));
  resource->last_ctx_id = 0;
  resource->last_ring_idx = 0;
  resource->last_fence_id = 0;
  resource->bo_handle = 0;
  spin_unlock(&virtio_gpu_backend->drm.virgl_lock);
  printk(LOG_INFO, "drm gem free virgl rid=%u vaddr=%p pages=%u\n", rid, vaddr,
         npages);
  virtio_gpu_resource_unref(rid);
  bfc_free_page_data(vaddr, npages);
}

static void drm_virgl_gem_release(struct drm_gem_object *object) {
  struct drm_virgl_resource *resource = drm_gem_object_private(object);
  BUG_ON(!resource || resource->gem != object);
  if (virtio_gpu_backend->event_wq)
    BUG_ON(!queue_work(virtio_gpu_backend->event_wq, &resource->release_work));
  else
    drm_virgl_release_work(&resource->release_work);
}

static const struct drm_gem_object_ops drm_dumb_gem_ops = {
    .release = drm_dumb_gem_release,
};

static const struct drm_gem_object_ops drm_virgl_gem_ops = {
    .release = drm_virgl_gem_release,
};

/* ===== Fence (plan2) ===== */

/* Register a common DRM fence in virtio's completion lookup table. */
static struct drm_fence *
virtio_drm_fence_create(uint32_t ctx_id, uint8_t ring_idx, uint64_t fence_id) {
  struct drm_fence *fence = drm_fence_create(false);
  if (!fence)
    return NULL;
  uint64_t flags;
  spin_lock_irqsave(&virtio_gpu_backend->drm.fence_lock, &flags);
  for (int i = 0; i < MAX_FENCES; i++) {
    struct drm_backend_fence_slot *slot = &virtio_gpu_backend->drm.fences[i];
    if (!slot->fence) {
      slot->ctx_id = ctx_id;
      slot->ring_idx = ring_idx;
      slot->fence_id = fence_id;
      slot->fence = fence;
      spin_unlock_irqrestore(&virtio_gpu_backend->drm.fence_lock, flags);
      return fence;
    }
  }
  spin_unlock_irqrestore(&virtio_gpu_backend->drm.fence_lock, flags);
  drm_fence_put(fence);
  return NULL;
}

/* Completion consumes the table's in-flight reference. */
static struct drm_fence *drm_fence_find(uint32_t ctx_id, uint8_t ring_idx,
                                        uint64_t fence_id) {
  uint64_t flags;
  spin_lock_irqsave(&virtio_gpu_backend->drm.fence_lock, &flags);
  for (int i = 0; i < MAX_FENCES; i++) {
    struct drm_backend_fence_slot *slot = &virtio_gpu_backend->drm.fences[i];
    if (slot->fence && slot->ctx_id == ctx_id && slot->ring_idx == ring_idx &&
        slot->fence_id == fence_id) {
      struct drm_fence *fence = slot->fence;
      __memset(slot, 0, sizeof(*slot));
      spin_unlock_irqrestore(&virtio_gpu_backend->drm.fence_lock, flags);
      return fence;
    }
  }
  spin_unlock_irqrestore(&virtio_gpu_backend->drm.fence_lock, flags);
  return NULL;
}

static void virtio_drm_fence_remove(struct drm_fence *fence) {
  uint64_t flags;
  spin_lock_irqsave(&virtio_gpu_backend->drm.fence_lock, &flags);
  for (int i = 0; i < MAX_FENCES; i++) {
    if (virtio_gpu_backend->drm.fences[i].fence == fence) {
      __memset(&virtio_gpu_backend->drm.fences[i], 0,
               sizeof(virtio_gpu_backend->drm.fences[i]));
      break;
    }
  }
  spin_unlock_irqrestore(&virtio_gpu_backend->drm.fence_lock, flags);
}

static void virtio_drm_fence_signal(struct drm_fence *fence, uint32_t ctx_id,
                                    uint8_t ring_idx, uint64_t fence_id) {
  if (!fence)
    return;
  uint64_t flags;
  spin_lock_irqsave(&virtio_gpu_backend->drm.fence_lock, &flags);
  if (ctx_id > 0 && ctx_id <= MAX_CTX_IDS && ring_idx < MAX_CTX_RINGS) {
    uint64_t *completed =
        &virtio_gpu_backend->drm.completed_fence_ids[ctx_id - 1][ring_idx];
    if (*completed < fence_id)
      *completed = fence_id;
  }
  spin_unlock_irqrestore(&virtio_gpu_backend->drm.fence_lock, flags);
  drm_fence_signal(fence);
}

static int drm_alloc_fb_id(void) {
  for (int i = 0; i < MAX_FRAMEBUFFERS; i++) {
    if (virtio_gpu_backend->drm.fbs[i].fb_id == 0) {
      virtio_gpu_backend->drm.fbs[i].fb_id = i + 1; /* fb_id = slot index + 1 */
      virtio_gpu_backend->drm.fbs[i].refcount = 1;
      return virtio_gpu_backend->drm.fbs[i].fb_id;
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
    spin_lock(&virtio_gpu_backend->drm.capset_lock);
    for (uint32_t i = 0; i < virtio_gpu_backend->drm.num_capsets; i++)
      val |= (1u << virtio_gpu_backend->drm.capsets[i].id);
    spin_unlock(&virtio_gpu_backend->drm.capset_lock);
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

/* DRM_IOCTL_VIRTGPU_GET_CAPS — return cached capset payload. addr is a
 * user-space pointer; copy up to c->size bytes. Serves any host-cached capset
 * (virgl id=1/2 when the host advertises them). An unknown id returns -EINVAL:
 * the virgl winsys checks errno==EINVAL to fall back from capset 2 (VIRGL2) to
 * capset 1 (VIRGL), so -ENOENT would break that path. */
static long drm_ioctl_virtgpu_get_caps(void *arg) {
  struct drm_virtgpu_get_caps *c = (struct drm_virtgpu_get_caps *)arg;

  const void *data = NULL;
  uint32_t data_size = 0;
  spin_lock(&virtio_gpu_backend->drm.capset_lock);
  for (uint32_t i = 0; i < virtio_gpu_backend->drm.num_capsets; i++) {
    if (virtio_gpu_backend->drm.capsets[i].id == c->cap_set_id) {
      data = virtio_gpu_backend->drm.capsets[i].data;
      data_size = virtio_gpu_backend->drm.capsets[i].size;
      break;
    }
  }
  spin_unlock(&virtio_gpu_backend->drm.capset_lock);
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
  spin_lock_irqsave(&virtio_gpu_backend->drm.fence_lock, &fence_flags);
  __memset(virtio_gpu_backend->drm.completed_fence_ids[ctx_id - 1], 0,
           sizeof(virtio_gpu_backend->drm.completed_fence_ids[ctx_id - 1]));
  spin_unlock_irqrestore(&virtio_gpu_backend->drm.fence_lock, fence_flags);

  struct virtio_gpu_ctx_create cmd;
  __memset(&cmd, 0, sizeof(cmd));
  cmd.hdr.type = VIRTIO_GPU_CMD_CTX_CREATE;
  cmd.hdr.ctx_id = ctx_id;
  cmd.nlen = 0;
  cmd.context_init = capset_id & VIRTIO_GPU_CONTEXT_INIT_CAPSET_ID_MASK;

  struct virtio_gpu_ctrl_hdr_response resp;
  __memset(&resp, 0, sizeof(resp));
  int rc = virtio_gpu_send_cmd_3d(&virtio_gpu_backend->vgpu, &cmd, sizeof(cmd),
                                  &resp, sizeof(resp));
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
    virtio_gpu_send_cmd_3d(&virtio_gpu_backend->vgpu, &destroy, sizeof(destroy),
                           &resp, sizeof(resp));
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
 * The bo_handle→res_handle mapping is persisted in
 * virtio_gpu_backend->drm.virgl_res[] so later TRANSFER_TO/FROM_HOST and WAIT
 * (which pass only bo_handle) can resolve it. */
static long drm_ioctl_virtgpu_resource_create(void *arg, struct drm_file *df,
                                              struct file *file) {
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
  printk(LOG_INFO, "drm gem alloc virgl handle=%u vaddr=%p pages=%u\n", handle,
         vaddr, npages);

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
  int rc2 = virtio_gpu_send_cmd_3d(&virtio_gpu_backend->vgpu, &cmd, sizeof(cmd),
                                   &resp, sizeof(resp));
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

  struct drm_virgl_resource *r =
      &virtio_gpu_backend->drm.virgl_res[handle - VIRGL_HANDLE_BASE];
  r->bo_handle = handle;
  r->res_handle = res_id;
  r->guest_phys = guest_phys;
  r->kernel_vaddr = vaddr;
  r->size = rc->size;
  r->refcount = 1;
  __memset(r->ctx_attach_bitmap, 0, sizeof(r->ctx_attach_bitmap));
  init_work(&r->release_work, drm_virgl_release_work);

  struct page **pages = drm_gem_contiguous_pages(guest_phys, npages);
  struct drm_gem_object *object =
      pages ? drm_gem_object_create(virtio_gpu_backend->core, r->size, pages,
                                    npages, r, &drm_virgl_gem_ops)
            : NULL;
  uint32_t file_handle = 0;
  if (object)
    r->gem = object;
  if (!object ||
      drm_core_gem_handle_create(file, object, handle, &file_handle)) {
    if (object) {
      drm_gem_object_put(object);
    } else {
      kfree(pages);
      virtio_gpu_resource_unref(res_id);
      bfc_free_page_data(vaddr, npages);
      free_virgl_handle(handle);
    }
    return -ENOMEM;
  }
  rc->bo_handle = file_handle;
  rc->res_handle = res_id;

  if (df && df->created_virgl_count < MAX_VIRGL_RESOURCES)
    df->created_virgl_handles[df->created_virgl_count++] = (int)handle;

  drm_gem_object_put(object);

  printk(LOG_DEBUG,
         "drm: RESOURCE_CREATE(v1) %ux%ux%u fmt=%u -> bo=%u res=%u\n",
         rc->width, rc->height, rc->depth, rc->format, handle, res_id);
  return 0;
}

static struct drm_virgl_resource *
drm_virgl_lookup_file(struct file *file, uint32_t handle,
                      struct drm_gem_object **object_out) {
  struct drm_gem_object *object = drm_core_gem_object_lookup(file, handle);
  if (!object)
    return NULL;
  struct drm_virgl_resource *resource = drm_gem_object_private(object);
  if (!resource || resource != drm_find_virgl_resource(handle)) {
    drm_gem_object_put(object);
    return NULL;
  }
  *object_out = object;
  return resource;
}

/* DRM_IOCTL_VIRTGPU_RESOURCE_INFO — return res_handle/size/blob_mem.
 * virgl legacy (v1) resources only; handles live at/above VIRGL_HANDLE_BASE. */
static long drm_ioctl_virtgpu_resource_info(void *arg, struct file *file) {
  struct drm_virtgpu_resource_info *ri =
      (struct drm_virtgpu_resource_info *)arg;

  struct drm_gem_object *object = NULL;
  struct drm_virgl_resource *r =
      drm_virgl_lookup_file(file, ri->bo_handle, &object);
  if (!r)
    return -EINVAL;

  ri->res_handle = r->res_handle;
  ri->size = (uint32_t)r->size;
  ri->blob_mem = 0;
  drm_gem_object_put(object);
  return 0;
}

/* DRM_IOCTL_VIRTGPU_MAP — return an mmap offset for a legacy virgl BO. */
static long drm_ioctl_virtgpu_map(void *arg, struct file *file) {
  struct drm_virtgpu_map *map = (struct drm_virtgpu_map *)arg;
  if (!map || !file)
    return -EFAULT;
  struct drm_gem_object *object = NULL;
  if (!drm_virgl_lookup_file(file, map->handle, &object))
    return -ENOENT;
  drm_gem_object_put(object);
  return drm_core_gem_mmap_offset(file, map->handle, &map->offset);
}

/* Make a host resource visible to a virgl context before SUBMIT_3D refers to
 * it. Mesa supplies the required GEM handles in EXECBUFFER.bo_handles. */
static int drm_virgl_attach_resource(uint32_t handle, uint32_t ctx_id) {
  uint32_t bit = ctx_id - 1;
  uint32_t word = bit / 32;
  uint32_t mask = 1u << (bit % 32);

  spin_lock(&virtio_gpu_backend->drm.virgl_lock);
  struct drm_virgl_resource *r = drm_find_virgl_resource(handle);
  if (!r) {
    spin_unlock(&virtio_gpu_backend->drm.virgl_lock);
    return -ENOENT;
  }
  if (r->ctx_attach_bitmap[word] & mask) {
    spin_unlock(&virtio_gpu_backend->drm.virgl_lock);
    return 0;
  }
  uint32_t resource_id = r->res_handle;
  spin_unlock(&virtio_gpu_backend->drm.virgl_lock);

  struct virtio_gpu_ctx_resource cmd;
  __memset(&cmd, 0, sizeof(cmd));
  cmd.hdr.type = VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE;
  cmd.hdr.ctx_id = ctx_id;
  cmd.resource_id = resource_id;

  struct virtio_gpu_ctrl_hdr_response resp;
  __memset(&resp, 0, sizeof(resp));
  int rc = virtio_gpu_send_cmd_3d(&virtio_gpu_backend->vgpu, &cmd, sizeof(cmd),
                                  &resp, sizeof(resp));
  if (rc || resp.hdr.type != VIRTIO_GPU_RESP_OK_NODATA)
    return rc ? rc : -EIO;

  spin_lock(&virtio_gpu_backend->drm.virgl_lock);
  r = drm_find_virgl_resource(handle);
  if (!r || r->res_handle != resource_id) {
    spin_unlock(&virtio_gpu_backend->drm.virgl_lock);
    return -ENOENT;
  }
  r->ctx_attach_bitmap[word] |= mask;
  spin_unlock(&virtio_gpu_backend->drm.virgl_lock);

  printk(LOG_DEBUG, "drm: context %u attached bo=%u resource=%u\n", ctx_id,
         handle, resource_id);
  return 0;
}

static void drm_virgl_forget_context(uint32_t ctx_id) {
  uint32_t bit = ctx_id - 1;
  uint32_t word = bit / 32;
  uint32_t mask = 1u << (bit % 32);

  spin_lock(&virtio_gpu_backend->drm.virgl_lock);
  for (uint32_t i = 0; i < MAX_VIRGL_RESOURCES; i++)
    virtio_gpu_backend->drm.virgl_res[i].ctx_attach_bitmap[word] &= ~mask;
  spin_unlock(&virtio_gpu_backend->drm.virgl_lock);
}

/* Build + send a 3D host transfer (TO/FROM) for a virgl v1 resource. The
 * winsys passes only bo_handle; resolve to the host res_handle here. v1
 * transfers are not context-bound on the host (ctx_id=0); virglrenderer
 * reaches the guest backing via the resource id. */
static long virgl_transfer_host_3d(void *arg, uint32_t cmd_type,
                                   struct file *file) {
  struct drm_virtgpu_3d_transfer_to_host *t =
      (struct drm_virtgpu_3d_transfer_to_host *)arg;
  /* drm_virtgpu_3d_transfer_from_host has an identical field layout. */

  struct drm_gem_object *object = NULL;
  struct drm_virgl_resource *r =
      drm_virgl_lookup_file(file, t->bo_handle, &object);
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
  int rc = virtio_gpu_send_cmd_3d(&virtio_gpu_backend->vgpu, &cmd, sizeof(cmd),
                                  &resp, sizeof(resp));
  drm_gem_object_put(object);
  if (rc || resp.hdr.type != VIRTIO_GPU_RESP_OK_NODATA)
    return rc ? rc : -EIO;
  return 0;
}

/* DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST — upload guest backing → host resource. */
static long drm_ioctl_virtgpu_transfer_to_host(void *arg, struct file *file) {
  return virgl_transfer_host_3d(arg, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D, file);
}

/* DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST — download host resource → guest
 * backing. */
static long drm_ioctl_virtgpu_transfer_from_host(void *arg, struct file *file) {
  return virgl_transfer_host_3d(arg, VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D,
                                file);
}

/* DRM_IOCTL_VIRTGPU_WAIT — virgl legacy busy/block query. `handle` is a GEM
 * bo_handle. v1 TRANSFERs are synchronous (send_cmd_3d blocks until the host
 * responds), so only in-flight EXECBUFFER (SUBMIT_3D) fences can make a
 * resource busy. Granularity is approximated to the owning context: a resource
 * is reported busy if any unsignaled fence for the current fd's context exists.
 * This is conservative (a different resource's submit may be in flight) but
 * never races — virgl only uses this to decide whether to stall. */
static long drm_ioctl_virtgpu_3d_wait(void *arg, struct drm_file *df,
                                      struct file *file) {
  struct drm_virtgpu_3d_wait *w = (struct drm_virtgpu_3d_wait *)arg;
  (void)df;

  bool nowait = w->flags & VIRTGPU_WAIT_NOWAIT;
  struct drm_gem_object *object = NULL;
  struct drm_virgl_resource *held_resource =
      drm_virgl_lookup_file(file, w->handle, &object);
  if (!held_resource)
    return -ENOENT;

  for (;;) {
    uint32_t ctx_id;
    uint8_t ring_idx;
    uint64_t fence_id;

    spin_lock(&virtio_gpu_backend->drm.virgl_lock);
    struct drm_virgl_resource *r = held_resource;
    ctx_id = r->last_ctx_id;
    ring_idx = r->last_ring_idx;
    fence_id = r->last_fence_id;
    spin_unlock(&virtio_gpu_backend->drm.virgl_lock);

    if (ctx_id == 0 || fence_id == 0) {
      drm_gem_object_put(object);
      return 0;
    }

    struct drm_fence *fence = NULL;
    uint64_t flags;
    spin_lock_irqsave(&virtio_gpu_backend->drm.fence_lock, &flags);
    if (ctx_id <= MAX_CTX_IDS && ring_idx < MAX_CTX_RINGS &&
        virtio_gpu_backend->drm.completed_fence_ids[ctx_id - 1][ring_idx] >=
            fence_id) {
      spin_unlock_irqrestore(&virtio_gpu_backend->drm.fence_lock, flags);
      drm_gem_object_put(object);
      return 0;
    }
    for (int i = 0; i < MAX_FENCES; i++) {
      struct drm_backend_fence_slot *slot = &virtio_gpu_backend->drm.fences[i];
      if (slot->fence && slot->ctx_id == ctx_id && slot->ring_idx == ring_idx &&
          slot->fence_id == fence_id) {
        fence = slot->fence;
        drm_fence_get(fence);
        break;
      }
    }
    spin_unlock_irqrestore(&virtio_gpu_backend->drm.fence_lock, flags);

    if (nowait) {
      drm_fence_put(fence);
      drm_gem_object_put(object);
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
static long drm_ioctl_virtgpu_execbuffer(void *arg, struct drm_file *df,
                                         struct file *file) {
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
  struct drm_gem_object **bo_objects = NULL;
  if (eb->num_bo_handles != 0) {
    size_t handles_size = eb->num_bo_handles * sizeof(*bo_handles);
    bo_handles = kmalloc(handles_size);
    if (!bo_handles)
      return -ENOMEM;
    bo_objects = kmalloc(eb->num_bo_handles * sizeof(*bo_objects));
    if (!bo_objects) {
      kfree(bo_handles);
      return -ENOMEM;
    }
    __memset(bo_objects, 0, eb->num_bo_handles * sizeof(*bo_objects));
    if (copy_from_user(bo_handles, (void *)(uintptr_t)eb->bo_handles,
                       handles_size)) {
      kfree(bo_handles);
      kfree(bo_objects);
      return -EFAULT;
    }

    /* Reject handles which aren't present in this DRM file before changing
     * host context state. This also excludes non-virgl GEM objects. */
    for (uint32_t i = 0; i < eb->num_bo_handles; i++) {
      if (!drm_virgl_lookup_file(file, bo_handles[i], &bo_objects[i])) {
        rc = -ENOENT;
        goto err_free_handles;
      }
    }

    for (uint32_t i = 0; i < eb->num_bo_handles; i++) {
      rc = drm_virgl_attach_resource(bo_handles[i], df->ctx_id);
      if (rc) {
        goto err_free_handles;
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
      virtio_drm_fence_create(df->ctx_id, eb->ring_idx, fence_id);
  if (!fence) {
    kfree(submit_buf);
    rc = -ENOMEM;
    goto err_free_handles;
  }

  for (uint32_t i = 0; i < eb->num_bo_handles; i++)
    drm_gem_reservation_set_exclusive(bo_objects[i], fence);

  rc = drm_fence_hold_objects(fence, bo_objects, eb->num_bo_handles);
  if (rc) {
    virtio_drm_fence_remove(fence);
    drm_fence_put(fence);
    kfree(submit_buf);
    goto err_free_handles;
  }
  bo_objects = NULL;

  /* Reserve a reference for the async completion before publishing the
   * descriptor. The creator's reference remains live until out-fence setup is
   * finished, even if the host completes immediately on another CPU. */
  drm_fence_get(fence);

  /* Submit async: send_cmd_3d_async copies submit_buf/resp into heap nodes
   * it owns, so freeing submit_buf here is safe. */
  struct virtio_gpu_ctrl_hdr_response resp_template;
  __memset(&resp_template, 0, sizeof(resp_template));
  rc = virtio_gpu_send_cmd_3d_async(
      &virtio_gpu_backend->vgpu, submit_buf, total_cmd_size, &resp_template,
      sizeof(resp_template), fence_id, eb->ring_idx, df->ctx_id);
  kfree(submit_buf);
  if (rc) {
    virtio_drm_fence_remove(fence);
    drm_fence_signal(fence);
    drm_fence_put(fence); /* unused async-completion reference */
    drm_fence_put(fence);
    goto err_free_handles;
  }
  perf_trace_causal(XOS_PERF_TRACE_IO, XOS_PERF_IO_SUBMIT,
                    0xc0000000U | (uint32_t)fence_id);

  spin_lock(&virtio_gpu_backend->drm.virgl_lock);
  for (uint32_t i = 0; i < eb->num_bo_handles; i++) {
    struct drm_virgl_resource *r = drm_find_virgl_resource(bo_handles[i]);
    if (!r)
      continue;
    r->last_ctx_id = df->ctx_id;
    r->last_ring_idx = eb->ring_idx;
    r->last_fence_id = fence_id;
  }
  spin_unlock(&virtio_gpu_backend->drm.virgl_lock);
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
  for (uint32_t i = 0; i < eb->num_bo_handles; i++)
    drm_gem_object_put(bo_objects ? bo_objects[i] : NULL);
  kfree(bo_objects);
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
/* DROP_MASTER 清理 — 重置 master 相关状态 */
static void drm_master_cleanup(void) {
  /* 1. Clear current FB (unbind CRTC scanout) */
  if (virtio_gpu_backend->drm.current_fb_id != 0) {
    virtio_gpu_backend->drm.current_fb_id = 0;
  }

  /* Disable cursor. Per-file events are cancelled by drm_core first. */
  virtio_gpu_backend->drm.cursor.enabled = false;
  virtio_gpu_backend->drm.cursor.dirty = false;
}

static void drm_master_drop(void *driver_private) {
  (void)driver_private;
  drm_master_cleanup();
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
  uint32_t w = virtio_gpu_backend->drm.fb_width,
           h = virtio_gpu_backend->drm.fb_height;
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
  printk(LOG_DEBUG, "drm_getresources: fb_w=%u fb_h=%u\n",
         virtio_gpu_backend->drm.fb_width, virtio_gpu_backend->drm.fb_height);
  r->count_crtcs = 1;
  r->count_connectors = 1;
  r->count_encoders = 1;
  r->min_width = virtio_gpu_backend->drm.fb_width;
  r->max_width = virtio_gpu_backend->drm.fb_width;
  r->min_height = virtio_gpu_backend->drm.fb_height;
  r->max_height = virtio_gpu_backend->drm.fb_height;

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
  spin_lock(&virtio_gpu_backend->drm.fb_lock);

  int count = 0;
  for (int i = 0; i < MAX_FRAMEBUFFERS; i++) {
    if (virtio_gpu_backend->drm.fbs[i].fb_id != 0)
      count++;
  }
  r->count_fbs = count;

  /* Fill fb ID buffer (second ioctl call) */
  if (count > 0 && r->fb_id_ptr) {
    uint32_t *fb_buf = (uint32_t *)kmalloc(count * sizeof(uint32_t));
    if (!fb_buf) {
      spin_unlock(&virtio_gpu_backend->drm.fb_lock);
      return -ENOMEM;
    }
    int idx = 0;
    for (int i = 0; i < MAX_FRAMEBUFFERS; i++) {
      if (virtio_gpu_backend->drm.fbs[i].fb_id != 0)
        fb_buf[idx++] = virtio_gpu_backend->drm.fbs[i].fb_id;
    }
    spin_unlock(&virtio_gpu_backend->drm.fb_lock);

    if (copy_to_user((void *)(uintptr_t)r->fb_id_ptr, fb_buf,
                     count * sizeof(uint32_t))) {
      kfree(fb_buf);
      return -EFAULT;
    }
    kfree(fb_buf);
  } else {
    spin_unlock(&virtio_gpu_backend->drm.fb_lock);
  }
  return 0;
}

/* DRM_IOCTL_MODE_GETCRTC */
static long drm_ioctl_getcrtc(void *arg) {
  struct drm_mode_crtc *c = (struct drm_mode_crtc *)arg;
  if (c->crtc_id != DRM_CRTC_ID)
    return -EINVAL;
  c->fb_id = virtio_gpu_backend->drm.current_fb_id;
  c->x = 0;
  c->y = 0;
  c->mode_valid = virtio_gpu_backend->drm.mode_valid ? 1 : 0;
  if (virtio_gpu_backend->drm.mode_valid) {
    struct drm_mode_modeinfo *m = &c->mode;
    __memset(m, 0, sizeof(*m));
    m->clock = 40000;
    m->hdisplay = virtio_gpu_backend->drm.fb_width;
    m->hsync_start = virtio_gpu_backend->drm.fb_width + 16;
    m->hsync_end = virtio_gpu_backend->drm.fb_width + 32;
    m->htotal = virtio_gpu_backend->drm.fb_width + 48;
    m->vdisplay = virtio_gpu_backend->drm.fb_height;
    m->vsync_start = virtio_gpu_backend->drm.fb_height + 1;
    m->vsync_end = virtio_gpu_backend->drm.fb_height + 4;
    m->vtotal = virtio_gpu_backend->drm.fb_height + 10;
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
    virtio_gpu_backend->drm.mode_valid = false;
    return 0;
  }
  struct drm_framebuffer *fb = drm_find_fb(c->fb_id);
  if (!fb)
    return -EINVAL;
  virtio_gpu_backend->drm.current_fb_id = c->fb_id;
  virtio_gpu_backend->drm.mode_valid = true;
  struct drm_dumb_buffer *d = drm_find_dumb(fb->dumb_handle);
  uint32_t resource_id =
      fb->is_imported ? fb->resource_id : (d ? d->virtio_res_id : 0);
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
    uint32_t props[2] = {0};
    uint64_t values[2] = {0};
    if (drm_core_object_property_by_name(
            virtio_gpu_backend->core, DRM_CONNECTOR_ID,
            DRM_MODE_OBJECT_CONNECTOR, "DPMS", &props[0], &values[0]) ||
        drm_core_object_property_by_name(
            virtio_gpu_backend->core, DRM_CONNECTOR_ID,
            DRM_MODE_OBJECT_CONNECTOR, "EDID", &props[1], &values[1]))
      return -ENOENT;
    if (copy_to_user((void *)(uintptr_t)c->props_ptr, props, sizeof(props)) ||
        copy_to_user((void *)(uintptr_t)c->prop_values_ptr, values,
                     sizeof(values)))
      return -EFAULT;
  }

  /* Fill mode data buffer (second ioctl call).
     Always report the default/configured mode as the connector's native
     capability, regardless of whether the CRTC has been set via SETCRTC
     yet. */
  if (c->modes_ptr) {
    struct drm_mode_modeinfo km;
    __memset(&km, 0, sizeof(km));
    km.clock = 40000;
    km.hdisplay = virtio_gpu_backend->drm.fb_width;
    km.hsync_start = virtio_gpu_backend->drm.fb_width + 16;
    km.hsync_end = virtio_gpu_backend->drm.fb_width + 32;
    km.htotal = virtio_gpu_backend->drm.fb_width + 48;
    km.vdisplay = virtio_gpu_backend->drm.fb_height;
    km.vsync_start = virtio_gpu_backend->drm.fb_height + 1;
    km.vsync_end = virtio_gpu_backend->drm.fb_height + 4;
    km.vtotal = virtio_gpu_backend->drm.fb_height + 10;
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
  p->fb_id = virtio_gpu_backend->drm.current_fb_id;
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
static long drm_ioctl_create_dumb(void *arg, struct file *file) {
  struct drm_mode_create_dumb *d = (struct drm_mode_create_dumb *)arg;
  if (d->width != virtio_gpu_backend->drm.fb_width ||
      d->height != virtio_gpu_backend->drm.fb_height ||
      d->bpp != virtio_gpu_backend->drm.fb_bpp)
    return -EINVAL;
  d->pitch = virtio_gpu_backend->drm.fb_pitch;
  d->size = (uint64_t)virtio_gpu_backend->drm.fb_pitch *
            virtio_gpu_backend->drm.fb_height;

  spin_lock(&virtio_gpu_backend->drm.dumb_lock);
  int handle = drm_alloc_dumb_handle();
  if (handle < 0) {
    spin_unlock(&virtio_gpu_backend->drm.dumb_lock);
    return -ENOMEM;
  }
  struct drm_dumb_buffer *buf = &virtio_gpu_backend->drm.dumbs[handle - 1];
  spin_unlock(&virtio_gpu_backend->drm.dumb_lock);

  buf->width = d->width;
  buf->height = d->height;
  buf->pitch = d->pitch;
  buf->size = d->size;

  uint32_t npages = (d->size + PAGE_SIZE - 1) / PAGE_SIZE;
  buf->kernel_vaddr = bfc_alloc_page_data(npages);
  if (!buf->kernel_vaddr) {
    spin_lock(&virtio_gpu_backend->drm.dumb_lock);
    __memset(buf, 0, sizeof(*buf));
    spin_unlock(&virtio_gpu_backend->drm.dumb_lock);
    return -ENOMEM;
  }
  /* PRIME mappings expose the last whole page, so keep its logical EOF tail
   * deterministic even when pitch * height is not page aligned. */
  __memset(buf->kernel_vaddr, 0, (size_t)npages * PAGE_SIZE);
  buf->guest_phys = (uint64_t)PHY_ADDR((uintptr_t)buf->kernel_vaddr);
  printk(LOG_INFO, "drm gem alloc dumb handle=%d vaddr=%p pages=%u\n", handle,
         buf->kernel_vaddr, npages);

  buf->virtio_res_id = (uint32_t)handle;
  if (virtio_gpu_create_2d(buf->virtio_res_id, d->width, d->height,
                           VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM) < 0) {
    bfc_free_page_data(buf->kernel_vaddr, npages);
    spin_lock(&virtio_gpu_backend->drm.dumb_lock);
    __memset(buf, 0, sizeof(*buf));
    spin_unlock(&virtio_gpu_backend->drm.dumb_lock);
    return -EIO;
  }
  if (virtio_gpu_attach_backing(buf->virtio_res_id, buf->guest_phys, d->size) <
      0) {
    virtio_gpu_resource_unref(buf->virtio_res_id);
    bfc_free_page_data(buf->kernel_vaddr, npages);
    spin_lock(&virtio_gpu_backend->drm.dumb_lock);
    __memset(buf, 0, sizeof(*buf));
    spin_unlock(&virtio_gpu_backend->drm.dumb_lock);
    return -EIO;
  }

  struct page **pages = drm_gem_contiguous_pages(buf->guest_phys, npages);
  init_work(&buf->release_work, drm_dumb_release_work);
  struct drm_gem_object *object =
      pages ? drm_gem_object_create(virtio_gpu_backend->core, buf->size, pages,
                                    npages, buf, &drm_dumb_gem_ops)
            : NULL;
  uint32_t file_handle = 0;
  if (object)
    buf->gem = object;
  if (!object || drm_core_gem_handle_create(file, object, (uint32_t)handle,
                                            &file_handle)) {
    if (object) {
      drm_gem_object_put(object);
    } else {
      kfree(pages);
      virtio_gpu_resource_unref(buf->virtio_res_id);
      bfc_free_page_data(buf->kernel_vaddr, npages);
      spin_lock(&virtio_gpu_backend->drm.dumb_lock);
      __memset(buf, 0, sizeof(*buf));
      spin_unlock(&virtio_gpu_backend->drm.dumb_lock);
    }
    return -ENOMEM;
  }
  d->handle = file_handle;
  drm_gem_object_put(object);

  return 0;
}

/* DRM_IOCTL_MODE_MAP_DUMB */
static long drm_ioctl_map_dumb(void *arg, struct file *file) {
  struct drm_mode_map_dumb *m = (struct drm_mode_map_dumb *)arg;
  return drm_core_gem_mmap_offset(file, m->handle, &m->offset);
}

/* DRM_IOCTL_MODE_DESTROY_DUMB */
static long drm_ioctl_destroy_dumb(void *arg, struct file *file) {
  struct drm_mode_destroy_dumb *d = (struct drm_mode_destroy_dumb *)arg;
  return drm_core_gem_handle_delete(file, d->handle);
}

/* DRM_IOCTL_GEM_CLOSE
 * Called by Mesa after ADDFB2 to release the handle reference.
 * In this simplified model, same semantics as DESTROY_DUMB. */
static long drm_ioctl_gem_close(void *arg, struct file *file) {
  struct drm_gem_close *c = (struct drm_gem_close *)arg;
  if (!c)
    return -EFAULT;

  return drm_core_gem_handle_delete(file, c->handle);
}

/* DRM_IOCTL_MODE_ADDFB */
static long drm_ioctl_addfb(void *arg, struct drm_file *cf, struct file *file) {
  struct drm_mode_fb_cmd *f = (struct drm_mode_fb_cmd *)arg;
  struct drm_gem_object *gem = drm_core_gem_object_lookup(file, f->handle);
  struct drm_dumb_buffer *d = gem ? drm_gem_object_private(gem) : NULL;
  if (!d || f->handle >= VIRGL_HANDLE_BASE) {
    drm_gem_object_put(gem);
    return -EINVAL;
  }

  spin_lock(&virtio_gpu_backend->drm.fb_lock);
  int fb_id = drm_alloc_fb_id();
  if (fb_id < 0) {
    spin_unlock(&virtio_gpu_backend->drm.fb_lock);
    drm_gem_object_put(gem);
    return -ENOMEM;
  }
  struct drm_framebuffer *fb = &virtio_gpu_backend->drm.fbs[fb_id - 1];
  spin_unlock(&virtio_gpu_backend->drm.fb_lock);

  fb->dumb_handle = (int)f->handle;
  fb->is_virgl = false;
  fb->gem = gem;
  fb->owner = cf;
  fb->width = f->width;
  fb->height = f->height;
  fb->pitch = f->pitch;
  fb->bpp = f->bpp;

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
static long drm_ioctl_addfb2(void *arg, struct drm_file *cf,
                             struct file *file) {
  struct drm_mode_fb_cmd2 *c = (struct drm_mode_fb_cmd2 *)arg;
  if (!c)
    return -EFAULT;

  for (unsigned i = 1; i < 4; i++) {
    if (c->handles[i] || c->pitches[i] || c->offsets[i] || c->modifier[i])
      return -EINVAL;
  }
  if (!c->handles[0] || c->offsets[0] != 0 || c->modifier[0] != 0)
    return -EINVAL;

  /* Validate pixel format */
  int bpp = bpp_from_format(c->pixel_format);
  if (bpp == 0)
    return -EINVAL;

  /* Validate flags (no modifiers currently) */
  if (c->flags != 0)
    return -EINVAL;

  bool is_virgl = c->handles[0] >= VIRGL_HANDLE_BASE;
  struct drm_gem_object *gem = drm_core_gem_object_lookup(file, c->handles[0]);
  void *backing = gem ? drm_gem_object_private(gem) : NULL;
  bool is_imported = gem && !backing;
  int backend_handle = (int)c->handles[0];
  if (!gem) {
    drm_gem_object_put(gem);
    return -ENOENT;
  }
  if (is_virgl) {
    struct drm_virgl_resource *r = drm_find_virgl_resource(c->handles[0]);
    if (!r || backing != r) {
      drm_gem_object_put(gem);
      return -ENOENT;
    }
    backend_handle = (int)r->bo_handle;
  } else if (!is_imported) {
    struct drm_dumb_buffer *d = backing;
    if (d->gem != gem || drm_find_dumb(d->handle) != d) {
      drm_gem_object_put(gem);
      return -ENOENT;
    }
    backend_handle = d->handle;
    if ((c->pixel_format != DRM_FORMAT_XRGB8888 &&
         c->pixel_format != DRM_FORMAT_ARGB8888) ||
        c->width != d->width || c->height != d->height ||
        c->pitches[0] != d->pitch ||
        (uint64_t)c->pitches[0] * c->height > drm_gem_object_size(gem)) {
      drm_gem_object_put(gem);
      return -EINVAL;
    }
  } else if ((c->pixel_format != DRM_FORMAT_XRGB8888 &&
              c->pixel_format != DRM_FORMAT_ARGB8888) ||
             c->pitches[0] < c->width * 4 ||
             (uint64_t)c->pitches[0] * c->height > drm_gem_object_size(gem)) {
    drm_gem_object_put(gem);
    return -EINVAL;
  }

  /* Allocate fb_id (shared with ADDFB) */
  spin_lock(&virtio_gpu_backend->drm.fb_lock);
  int fb_id = drm_alloc_fb_id();
  if (fb_id < 0) {
    spin_unlock(&virtio_gpu_backend->drm.fb_lock);
    drm_gem_object_put(gem);
    return -ENOMEM;
  }
  struct drm_framebuffer *fb = &virtio_gpu_backend->drm.fbs[fb_id - 1];
  spin_unlock(&virtio_gpu_backend->drm.fb_lock);

  if (is_imported) {
    uint32_t page_count = 0;
    struct page **pages = drm_gem_object_pages(gem, &page_count);
    uint64_t first_phys =
        pages && page_count ? (__force uint64_t)page_to_phys(pages[0]) : 0;
    bool contiguous = pages && page_count;
    for (uint32_t i = 1; contiguous && i < page_count; i++)
      contiguous = (__force uint64_t)page_to_phys(pages[i]) ==
                   first_phys + (uint64_t)i * PAGE_SIZE;
    fb->resource_id = 0x80000000u | (uint32_t)fb_id;
    bool resource_created = false;
    if (contiguous &&
        virtio_gpu_create_2d(fb->resource_id, c->width, c->height,
                             VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM) == 0)
      resource_created = true;
    if (!resource_created ||
        virtio_gpu_attach_backing(fb->resource_id, first_phys,
                                  (uint32_t)drm_gem_object_size(gem)) < 0) {
      if (resource_created)
        virtio_gpu_resource_unref(fb->resource_id);
      __memset(fb, 0, sizeof(*fb));
      drm_gem_object_put(gem);
      return contiguous ? -EIO : -EINVAL;
    }
  }

  fb->dumb_handle = backend_handle;
  fb->is_virgl = is_virgl;
  fb->is_imported = is_imported;
  fb->gem = gem;
  fb->owner = cf;
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
  spin_lock(&virtio_gpu_backend->drm.fb_lock);
  struct drm_framebuffer *fb = drm_find_fb((int)fb_id);
  if (!fb) {
    spin_unlock(&virtio_gpu_backend->drm.fb_lock);
    return -EINVAL;
  }
  fb->refcount--;
  struct drm_gem_object *gem = fb->gem;
  uint32_t imported_resource = fb->is_imported ? fb->resource_id : 0;
  if (fb->refcount <= 0) {
    __memset(fb, 0, sizeof(*fb));
  }
  spin_unlock(&virtio_gpu_backend->drm.fb_lock);

  if (imported_resource)
    virtio_gpu_resource_unref(imported_resource);
  drm_gem_object_put(gem);
  return 0;
}

/* ===== Cursor overlay (Phase C) ===== */
static void drm_cursor_overlay(struct drm_dumb_buffer *target) {
  if (!virtio_gpu_backend->drm.cursor.dirty ||
      !virtio_gpu_backend->drm.cursor.enabled)
    return;

  uint32_t *fb = (uint32_t *)target->kernel_vaddr;
  int fb_w = (int)target->width;
  int fb_h = (int)target->height;

  int sx = (virtio_gpu_backend->drm.cursor.x -
            virtio_gpu_backend->drm.cursor.hotspot_x);
  int sy = (virtio_gpu_backend->drm.cursor.y -
            virtio_gpu_backend->drm.cursor.hotspot_y);

  for (int cy = 0; cy < CURSOR_HEIGHT; cy++) {
    for (int cx = 0; cx < CURSOR_WIDTH; cx++) {
      int fx = sx + cx, fy = sy + cy;
      if (fx < 0 || fx >= fb_w || fy < 0 || fy >= fb_h)
        continue;

      uint32_t cpixel =
          virtio_gpu_backend->drm.cursor.buffer[cy * CURSOR_WIDTH + cx];
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

  virtio_gpu_backend->drm.cursor.dirty = false;
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
      virtio_gpu_backend->drm.cursor.enabled = false;
      virtio_gpu_backend->drm.cursor.dirty = true;
      return 0;
    }
    /* Set cursor bitmap: c->handle is a dumb buffer handle containing cursor
     * image data */
    spin_lock(&virtio_gpu_backend->drm.dumb_lock);
    struct drm_dumb_buffer *d = drm_find_dumb((int)c->handle);
    if (!d || d->size < CURSOR_SIZE) {
      spin_unlock(&virtio_gpu_backend->drm.dumb_lock);
      return -EINVAL;
    }
    __memcpy(virtio_gpu_backend->drm.cursor.buffer, d->kernel_vaddr,
             CURSOR_SIZE);
    spin_unlock(&virtio_gpu_backend->drm.dumb_lock);
    virtio_gpu_backend->drm.cursor.hotspot_x = (int16_t)c->hot_x;
    virtio_gpu_backend->drm.cursor.hotspot_y = (int16_t)c->hot_y;
    virtio_gpu_backend->drm.cursor.enabled = true;
    virtio_gpu_backend->drm.cursor.dirty = true;
    return 0;
  }
  case DRM_MODE_CURSOR_MOVE:
    virtio_gpu_backend->drm.cursor.x = (int16_t)c->x;
    virtio_gpu_backend->drm.cursor.y = (int16_t)c->y;
    virtio_gpu_backend->drm.cursor.dirty = true;
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
static long drm_ioctl_page_flip(struct file *file, void *arg) {
  struct drm_mode_crtc_page_flip *p = (struct drm_mode_crtc_page_flip *)arg;
  if (p->crtc_id != DRM_CRTC_ID)
    return -EINVAL;
  struct drm_framebuffer *fb = drm_find_fb((int)p->fb_id);
  if (!fb)
    return -EINVAL;
  struct drm_dumb_buffer *d = drm_find_dumb(fb->dumb_handle);
  uint32_t resource_id =
      fb->is_imported ? fb->resource_id : (d ? d->virtio_res_id : 0);
  if (fb->is_virgl) {
    struct drm_virgl_resource *r =
        drm_find_virgl_resource((uint32_t)fb->dumb_handle);
    resource_id = r ? r->res_handle : 0;
  }
  if (!resource_id)
    return -EINVAL;

  bool armed_event = false;
  if (p->flags & DRM_MODE_PAGE_FLIP_EVENT) {
    int rc = drm_core_event_queue(file, p->user_data, p->crtc_id,
                                  sched_clock() + DRM_VBLANK_INTERVAL_NS);
    if (rc)
      return rc;
    armed_event = true;
  }

  if (d) {
    drm_cursor_overlay(d);
    virtio_gpu_transfer_2d(resource_id, 0, 0, d->width, d->height, 0);
  } else if (fb->is_imported) {
    virtio_gpu_transfer_2d(resource_id, 0, 0, fb->width, fb->height, 0);
  }
  virtio_gpu_set_scanout(0, resource_id, 0, 0, fb->width, fb->height);
  virtio_gpu_flush(resource_id, 0, 0, fb->width, fb->height);

  virtio_gpu_backend->drm.current_fb_id = p->fb_id;
  if (armed_event && drm_page_flip_log_count < 3) {
    drm_page_flip_log_count++;
    printk(LOG_DEBUG, "drm: page flip #%u queued fb=%u resource=%u\n",
           drm_page_flip_log_count, p->fb_id, resource_id);
  }
  return 0;
}

static void virtio_gpu_event_work(struct work *work) {
  (void)work;
  if (!virtio_gpu_backend || !virtio_gpu_backend->drm.initialized ||
      !virtio_gpu_backend->core)
    return;

  drm_core_event_tick(virtio_gpu_backend->core, sched_clock());
}

void virtio_gpu_poll(void) {
  if (!virtio_gpu_backend || !virtio_gpu_backend->drm.initialized ||
      !virtio_gpu_backend->event_wq)
    return;

  /* timer_poll_hook runs in hard-IRQ context; file_mutex may sleep. A single
   * work item coalesces ticks while queued/running and performs the scan from
   * a kthread where taking the mutex is legal. */
  queue_work(virtio_gpu_backend->event_wq, &virtio_gpu_backend->event_work);
}

/* DRM_IOCTL_MODE_DIRTYFB */
static long drm_ioctl_dirtyfb(void *arg) {
  struct drm_mode_fb_dirty_cmd *c = (struct drm_mode_fb_dirty_cmd *)arg;
  struct drm_framebuffer *fb = drm_find_fb((int)c->fb_id);
  if (!fb)
    return -EINVAL;
  struct drm_dumb_buffer *d = drm_find_dumb(fb->dumb_handle);
  uint32_t resource_id =
      fb->is_imported ? fb->resource_id : (d ? d->virtio_res_id : 0);
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
  } else if (fb->is_imported) {
    virtio_gpu_transfer_2d(resource_id, 0, 0, fb->width, fb->height, 0);
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
  if (!virtio_gpu_backend ||
      !__atomic_load_n(&virtio_gpu_backend->hardware_live, __ATOMIC_ACQUIRE))
    return -ENODEV;
  printk(LOG_DEBUG, "drm_ioctl: cmd=0x%x initialized=%d\n", cmd,
         virtio_gpu_backend->drm.initialized);
  if (!virtio_gpu_backend->drm.initialized)
    return -ENODEV;
  switch (cmd) {
  case DRM_IOCTL_MODE_SETCRTC:
  case DRM_IOCTL_MODE_PAGE_FLIP:
  case DRM_IOCTL_MODE_CURSOR:
  case DRM_IOCTL_MODE_CURSOR2:
  case DRM_IOCTL_MODE_DIRTYFB:
  case DRM_IOCTL_MODE_SETPROPERTY:
  case DRM_IOCTL_MODE_OBJ_SETPROPERTY:
    if (!drm_core_file_is_master(file))
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
    return drm_ioctl_virtgpu_resource_create(arg, df, file);
  case DRM_IOCTL_VIRTGPU_RESOURCE_INFO:
    return drm_ioctl_virtgpu_resource_info(arg, file);
  case DRM_IOCTL_VIRTGPU_MAP:
    return drm_ioctl_virtgpu_map(arg, file);
  case DRM_IOCTL_VIRTGPU_EXECBUFFER:
    return drm_ioctl_virtgpu_execbuffer(arg, df, file);
  case DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST:
    return drm_ioctl_virtgpu_transfer_to_host(arg, file);
  case DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST:
    return drm_ioctl_virtgpu_transfer_from_host(arg, file);
  case DRM_IOCTL_VIRTGPU_WAIT:
    return drm_ioctl_virtgpu_3d_wait(arg, df, file);
  case DRM_IOCTL_GET_CAP:
    return drm_ioctl_get_cap(arg);
  case DRM_IOCTL_SET_CLIENT_CAP:
    return -ENOTTY; /* handled by drm_core's common ioctl table */
  case DRM_IOCTL_SET_MASTER:
    return -ENOTTY;
  case DRM_IOCTL_DROP_MASTER:
    return -ENOTTY;
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
    return drm_ioctl_create_dumb(arg, file);
  case DRM_IOCTL_MODE_MAP_DUMB:
    return drm_ioctl_map_dumb(arg, file);
  case DRM_IOCTL_MODE_DESTROY_DUMB:
    return drm_ioctl_destroy_dumb(arg, file);
  case DRM_IOCTL_MODE_ADDFB:
    return drm_ioctl_addfb(arg, df, file);
  case DRM_IOCTL_MODE_ADDFB2:
    return drm_ioctl_addfb2(arg, df, file);
  case DRM_IOCTL_MODE_RMFB:
    return drm_ioctl_rmfb(arg);
  case DRM_IOCTL_MODE_PAGE_FLIP:
    return drm_ioctl_page_flip(file, arg);
  case DRM_IOCTL_MODE_DIRTYFB:
    return drm_ioctl_dirtyfb(arg);
  case DRM_IOCTL_MODE_CURSOR:
    return drm_ioctl_cursor(arg);
  case DRM_IOCTL_MODE_CURSOR2:
    return drm_ioctl_cursor2(arg);
  case DRM_IOCTL_MODE_GETFB:
    return drm_ioctl_getfb(arg);
  case DRM_IOCTL_GET_MAGIC:
    return -ENOTTY;
  case DRM_IOCTL_AUTH_MAGIC:
    return -ENOTTY;
  case DRM_IOCTL_GEM_CLOSE:
    return drm_ioctl_gem_close(arg, file);
  case DRM_IOCTL_MODE_GETPROPERTY:
    return -ENOTTY;
  case DRM_IOCTL_MODE_SETPROPERTY:
    return -ENOTTY;
  case DRM_IOCTL_MODE_OBJ_SETPROPERTY:
    return -ENOTTY;
  case DRM_IOCTL_MODE_GETPROPBLOB:
    return -ENOTTY;
  case DRM_IOCTL_MODE_OBJ_GETPROPERTIES:
    return -ENOTTY;
  case DRM_IOCTL_MODE_CREATE_LEASE:
    /* Empty leases are optional. wlroots falls back to reopening the node. */
    return -EOPNOTSUPP;
  case DRM_IOCTL_PRIME_HANDLE_TO_FD:
    return -ENOTTY;
  case DRM_IOCTL_PRIME_FD_TO_HANDLE:
    return -ENOTTY;
  default:
    printk(LOG_WARN, "drm_ioctl: unknown cmd 0x%x\n", cmd);
    return -ENOSYS;
  }
}

/* ctx_id pool: ctx_id 0 is reserved ("no context"), ids 1..MAX_CTX_IDS. */
static uint32_t alloc_ctx_id(void) {
  spin_lock(&virtio_gpu_backend->drm.ctx_id_lock);
  for (uint32_t i = 0; i < MAX_CTX_IDS; i++) {
    if (!(virtio_gpu_backend->drm.ctx_id_bitmap[i / 32] & (1u << (i % 32)))) {
      virtio_gpu_backend->drm.ctx_id_bitmap[i / 32] |= (1u << (i % 32));
      spin_unlock(&virtio_gpu_backend->drm.ctx_id_lock);
      return i + 1;
    }
  }
  spin_unlock(&virtio_gpu_backend->drm.ctx_id_lock);
  return 0;
}

static void free_ctx_id(uint32_t id) {
  if (id == 0 || id > MAX_CTX_IDS)
    return;
  spin_lock(&virtio_gpu_backend->drm.ctx_id_lock);
  virtio_gpu_backend->drm.ctx_id_bitmap[(id - 1) / 32] &=
      ~(1u << ((id - 1) % 32));
  spin_unlock(&virtio_gpu_backend->drm.ctx_id_lock);
}

/* blob handle: monotonic 1-based; slot reuse keyed by bo_handle. */
/* Virgl v1 handles mirror table indices. Probe from the last allocation so
 * GEM_CLOSE slots are reusable instead of permanently exhausting the pool. */
static uint32_t alloc_virgl_handle(void) {
  spin_lock(&virtio_gpu_backend->drm.virgl_lock);
  uint32_t start =
      virtio_gpu_backend->drm.next_virgl_handle - VIRGL_HANDLE_BASE;
  for (uint32_t i = 0; i < MAX_VIRGL_RESOURCES; i++) {
    uint32_t index = (start + i) % MAX_VIRGL_RESOURCES;
    struct drm_virgl_resource *r = &virtio_gpu_backend->drm.virgl_res[index];
    if (r->bo_handle != 0 || r->release_work.state != WORK_IDLE)
      continue;

    uint32_t h = VIRGL_HANDLE_BASE + index;
    r->bo_handle = h; /* reserve the slot until resource creation completes */
    virtio_gpu_backend->drm.next_virgl_handle =
        VIRGL_HANDLE_BASE + ((index + 1) % MAX_VIRGL_RESOURCES);
    spin_unlock(&virtio_gpu_backend->drm.virgl_lock);
    return h;
  }
  spin_unlock(&virtio_gpu_backend->drm.virgl_lock);
  return 0;
}

static struct drm_virgl_resource *drm_find_virgl_resource(uint32_t handle) {
  if (handle < VIRGL_HANDLE_BASE ||
      handle >= VIRGL_HANDLE_BASE + MAX_VIRGL_RESOURCES)
    return NULL;
  struct drm_virgl_resource *r =
      &virtio_gpu_backend->drm.virgl_res[handle - VIRGL_HANDLE_BASE];
  return (r->bo_handle == handle) ? r : NULL;
}

static void free_virgl_handle(uint32_t handle) {
  if (handle < VIRGL_HANDLE_BASE ||
      handle >= VIRGL_HANDLE_BASE + MAX_VIRGL_RESOURCES)
    return;
  struct drm_virgl_resource *r =
      &virtio_gpu_backend->drm.virgl_res[handle - VIRGL_HANDLE_BASE];
  if (r->bo_handle != handle)
    return;
  __memset(r, 0, sizeof(*r));
}

/* True if capset_id was cached from the host. */
static bool virgl_capset_present(uint32_t capset_id) {
  bool found = false;
  spin_lock(&virtio_gpu_backend->drm.capset_lock);
  for (uint32_t i = 0; i < virtio_gpu_backend->drm.num_capsets; i++) {
    if (virtio_gpu_backend->drm.capsets[i].id == capset_id) {
      found = true;
      break;
    }
  }
  spin_unlock(&virtio_gpu_backend->drm.capset_lock);
  return found;
}

/* ===== DRM device ops ===== */
static int drm_files_grow_locked(void) {
  if (virtio_gpu_backend->files_capacity >= DRM_FD_MAX_CAPACITY)
    return -ENFILE;

  int new_capacity = virtio_gpu_backend->files_capacity
                         ? virtio_gpu_backend->files_capacity * 2
                         : DRM_FD_INITIAL_CAPACITY;
  if (new_capacity > DRM_FD_MAX_CAPACITY)
    new_capacity = DRM_FD_MAX_CAPACITY;

  struct drm_file **new_slots =
      kmalloc(sizeof(struct drm_file *) * (size_t)new_capacity);
  if (!new_slots)
    return -ENOMEM;
  __memset(new_slots, 0, sizeof(struct drm_file *) * (size_t)new_capacity);
  if (virtio_gpu_backend->files_capacity > 0) {
    __memcpy(new_slots, virtio_gpu_backend->files,
             sizeof(struct drm_file *) *
                 (size_t)virtio_gpu_backend->files_capacity);
  }
  kfree(virtio_gpu_backend->files);
  virtio_gpu_backend->files = new_slots;
  virtio_gpu_backend->files_capacity = new_capacity;
  return 0;
}

static int drm_open_file_common(xtask *proc, struct file *file,
                                bool is_render) {
  spin_lock(&virtio_gpu_backend->files_lock);
  int slot = -1;
  for (int i = 0; i < virtio_gpu_backend->files_capacity; i++) {
    if (!virtio_gpu_backend->files[i]) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    int old_capacity = virtio_gpu_backend->files_capacity;
    int ret = drm_files_grow_locked();
    if (ret) {
      spin_unlock(&virtio_gpu_backend->files_lock);
      printk(LOG_ERROR, "drm: cannot grow open-file table capacity=%d ret=%d\n",
             old_capacity, ret);
      return ret;
    }
    slot = old_capacity;
  }

  struct drm_file *drm_file = kmalloc(sizeof(*drm_file));
  if (!drm_file) {
    spin_unlock(&virtio_gpu_backend->files_lock);
    return -ENOMEM;
  }
  __memset(drm_file, 0, sizeof(*drm_file));
  drm_file->used = true;
  drm_file->fd = -1;
  drm_file->proc = proc;
  drm_file->is_render = is_render;
  virtio_gpu_backend->files[slot] = drm_file;
  file->private_data = drm_file;

  int used_count = 0;
  for (int i = 0; i < virtio_gpu_backend->files_capacity; i++)
    if (virtio_gpu_backend->files[i])
      used_count++;
  int capacity = virtio_gpu_backend->files_capacity;
  spin_unlock(&virtio_gpu_backend->files_lock);
  printk(LOG_DEBUG, "drm: open slot=%d pid=%d render=%d used=%d/%d\n", slot,
         proc ? proc->pid : -1, is_render, used_count, capacity);
  return 0;
}

static int drm_open_file(xtask *proc, struct file *file) {
  return drm_open_file_common(proc, file, false);
}

/* Render node open shares the backend file table with the primary node, but
 * marks is_render. Render fds reject SET_MASTER/GET_MAGIC/AUTH_MAGIC. */
static int drm_render_open_file(xtask *proc, struct file *file) {
  return drm_open_file_common(proc, file, true);
}

/* Helper: release a framebuffer (refcount decrement + cleanup) */
static void drm_release_fb(int fb_id, struct drm_file *owner) {
  spin_lock(&virtio_gpu_backend->drm.fb_lock);
  struct drm_framebuffer *fb = drm_find_fb(fb_id);
  if (!fb || (owner && fb->owner != owner)) {
    spin_unlock(&virtio_gpu_backend->drm.fb_lock);
    return;
  }
  struct drm_gem_object *gem = fb->gem;
  uint32_t imported_resource = fb->is_imported ? fb->resource_id : 0;
  fb->refcount--;
  if (fb->refcount <= 0) {
    __memset(fb, 0, sizeof(*fb));
  }
  spin_unlock(&virtio_gpu_backend->drm.fb_lock);

  if (imported_resource)
    virtio_gpu_resource_unref(imported_resource);
  drm_gem_object_put(gem);
}

static int drm_close_file(xtask *proc, struct file *file) {
  struct drm_file *target = file ? (struct drm_file *)file->private_data : NULL;
  if (!target)
    return 0;
  int close_slot = -1;
  int used_count = 0;
  int capacity = 0;
  spin_lock(&virtio_gpu_backend->files_lock);
  for (int i = 0; i < virtio_gpu_backend->files_capacity; i++) {
    struct drm_file *f = virtio_gpu_backend->files[i];
    if (!f || !f->used || f != target)
      continue;
    close_slot = i;
    virtio_gpu_backend->files[i] = NULL;
    file->private_data = NULL;
    for (int j = 0; j < virtio_gpu_backend->files_capacity; j++)
      if (virtio_gpu_backend->files[j])
        used_count++;
    capacity = virtio_gpu_backend->files_capacity;
    spin_unlock(&virtio_gpu_backend->files_lock);
    goto detached;
  }
  spin_unlock(&virtio_gpu_backend->files_lock);
  return 0; /* ignore close on unknown fd */

detached:
  for (int j = 0; j < target->created_fb_count; j++)
    drm_release_fb(target->created_fb_ids[j], target);

  if (target->ctx_id != 0) {
    struct virtio_gpu_ctx_create destroy;
    struct virtio_gpu_ctrl_hdr_response resp;
    __memset(&destroy, 0, sizeof(destroy));
    __memset(&resp, 0, sizeof(resp));
    destroy.hdr.type = VIRTIO_GPU_CMD_CTX_DESTROY;
    destroy.hdr.ctx_id = target->ctx_id;
    virtio_gpu_send_cmd_3d(&virtio_gpu_backend->vgpu, &destroy, sizeof(destroy),
                           &resp, sizeof(resp));
    drm_virgl_forget_context(target->ctx_id);
    free_ctx_id(target->ctx_id);
  }
  kfree(target->ring_fence_counters);
  kfree(target);
  printk(LOG_DEBUG, "drm: close slot=%d pid=%d used=%d/%d\n", close_slot,
         proc ? proc->pid : -1, used_count, capacity);
  return 0;
}

static const struct dev_ops virtio_gpu_primary_ops = {
    .driver_pid = 0,
    .is_block = false,
    .open_file = drm_open_file,
    .close_file = drm_close_file,
    .ioctl_file = drm_ioctl_file,
};

/* Render node ops: shares ioctl/mmap/close with the paired primary node and
 * rejects read/poll (no vblank events on render nodes). */
static const struct dev_ops virtio_gpu_render_ops = {
    .driver_pid = 0,
    .is_block = false,
    .open_file = drm_render_open_file,
    .close_file = drm_close_file,
    .ioctl_file = drm_ioctl_file,
    .read = NULL,
    .poll = NULL,
};

/* DRM PCI 设备访问 (设计 C1) */
static struct pci_device *drm_pci_dev(void) {
  return virtio_gpu_backend->vgpu.vpci.pdev;
}

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
static ssize_t drm_show_enabled(char *buf, size_t len, void *priv) {
  (void)priv;
  return snprintf(buf, len, "%d\n",
                  virtio_gpu_backend->drm.initialized ? 1 : 0);
}
static ssize_t drm_show_mode(char *buf, size_t len, void *priv) {
  (void)priv;
  return snprintf(buf, len, "%ux%u\n", virtio_gpu_backend->drm.fb_width,
                  virtio_gpu_backend->drm.fb_height);
}
static ssize_t drm_show_connector_status(char *buf, size_t len, void *priv) {
  (void)priv;
  return snprintf(buf, len, "connected\n");
}
static ssize_t drm_show_num_scanouts(char *buf, size_t len, void *priv) {
  (void)priv;
  return snprintf(buf, len, "%u\n",
                  virtio_gpu_backend->vgpu.config.num_scanouts);
}

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

static bool drm_dev_alloc(void) {
  if (virtio_gpu_backend->core)
    return true;
  const struct drm_core_config config = {
      .driver_name = "virtio_gpu",
      .subsystem_target = "/sys/bus/virtio",
      .primary_ops = &virtio_gpu_primary_ops,
      .render_ops = &virtio_gpu_render_ops,
      .driver_private = virtio_gpu_backend,
      .master_drop = drm_master_drop,
      .driver_release = virtio_gpu_backend_release,
  };
  virtio_gpu_backend->core = drm_core_device_alloc(&config);
  if (!virtio_gpu_backend->core) {
    printk(LOG_ERROR, "drm: failed to allocate virtio DRM device\n");
    return false;
  }
  return true;
}

static int drm_dev_register(void) {
  if (!drm_dev_alloc())
    return -ENOMEM;
  int rc = drm_core_device_register(virtio_gpu_backend->core,
                                    DRM_NODE_PRIMARY | DRM_NODE_RENDER);
  if (rc) {
    printk(LOG_ERROR, "drm: failed to register virtio DRM device: %d\n", rc);
    return rc;
  }

  struct sysfs_node *primary_node =
      drm_core_class_node(virtio_gpu_backend->core, DRM_NODE_PRIMARY);
  if (primary_node) {
    sysfs_create_file(primary_node, "vendor", &drm_attr_vendor);
    sysfs_create_file(primary_node, "device", &drm_attr_device);
    sysfs_create_file(primary_node, "class", &drm_attr_class);
    sysfs_create_file(primary_node, "enabled", &drm_attr_enabled);
    sysfs_create_file(primary_node, "mode", &drm_attr_mode);
    sysfs_create_file(primary_node, "connector_status",
                      &drm_attr_connector_status);
    sysfs_create_file(primary_node, "num_scanouts", &drm_attr_num_scanouts);
  }

  struct sysfs_node *rnode =
      drm_core_class_node(virtio_gpu_backend->core, DRM_NODE_RENDER);
  if (rnode) {
    sysfs_create_file(rnode, "vendor", &drm_attr_vendor);
    sysfs_create_file(rnode, "device", &drm_attr_device);
    sysfs_create_file(rnode, "class", &drm_attr_class);
  }
  struct sysfs_node *card_devchar =
      drm_core_devchar_node(virtio_gpu_backend->core, DRM_NODE_PRIMARY);
  struct sysfs_node *render_devchar =
      drm_core_devchar_node(virtio_gpu_backend->core, DRM_NODE_RENDER);
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
  printk(LOG_INFO, "drm: registered virtio DRM nodes at slot %d\n",
         drm_core_device_slot(virtio_gpu_backend->core));
  return 0;
}

/* Pre-query all capsets via GET_CAPSET_INFO + GET_CAPSET and cache them in
 * virtio_gpu_backend->drm.capsets[]. The bitmask surfaced by
 * GETPARAM(SUPPORTED_CAPSET_IDs) and the capsets served by GET_CAPS reflect
 * exactly what the host advertises — nothing is synthesized. The Venus path is
 * retired; only host-provided virgl capsets (1/2) drive the GL winsys. */
static void drm_query_capsets(struct virtio_gpu_device *vgpu) {
  virtio_gpu_backend->drm.capset_lock = SPINLOCK_INIT;
  uint32_t n = vgpu->config.num_capsets;
  if (n > MAX_CAPSETS)
    n = MAX_CAPSETS;
  virtio_gpu_backend->drm.num_capsets = 0;

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

    uint32_t slot = virtio_gpu_backend->drm.num_capsets;
    virtio_gpu_backend->drm.capsets[slot].id = info_resp.capset_id;
    virtio_gpu_backend->drm.capsets[slot].ver = info_resp.capset_max_version;
    virtio_gpu_backend->drm.capsets[slot].size = csz;
    virtio_gpu_backend->drm.capsets[slot].data = cdata;
    virtio_gpu_backend->drm.num_capsets++;
    printk(LOG_INFO, "drm: capset id=%u version=%u size=%u\n",
           info_resp.capset_id, info_resp.capset_max_version, csz);
  }

  printk(LOG_INFO, "drm: cached %u capsets\n",
         virtio_gpu_backend->drm.num_capsets);
}

static int virtio_gpu_probe(pci_device *pdev, const struct pci_device_id *id) {
  (void)id;
  if (virtio_gpu_backend)
    return -EBUSY;
  int rc = 0;
  const char *stage = "backend-allocation";
  struct virtio_gpu_backend *backend = kmalloc(sizeof(*backend));
  if (!backend)
    return -ENOMEM;
  __memset(backend, 0, sizeof(*backend));
  backend->files_lock = SPINLOCK_INIT;
  virtio_gpu_backend = backend;
  pci_set_driver_private(pdev, backend);

  struct virtio_gpu_device *vgpu = &virtio_gpu_backend->vgpu;
  __memset(vgpu, 0, sizeof(*vgpu));
  vgpu->cmd_lock = SPINLOCK_INIT;
  vgpu->pending_lock = SPINLOCK_INIT;
  vgpu->pending_list = NULL;

  printk(LOG_INFO, "virtio_gpu: found PCI device bus=%d dev=%d func=%d\n",
         pdev->bus, pdev->dev, pdev->func);

  /* Initialize transport */
  stage = "transport-init";
  rc = virtio_pci_init(&vgpu->vpci, pdev);
  if (rc < 0)
    goto fail;

  /* Negotiate features: VERSION_1 + VIRGL(3D/context) + CONTEXT_INIT(multi-
   * ring). The blob feature (Venus resource model) is retired; virgl uses the
   * legacy v1 RESOURCE_CREATE path and does not need it. */
  uint64_t want = (1ULL << VIRTIO_F_VERSION_1) | (1ULL << VIRTIO_GPU_F_VIRGL) |
                  (1ULL << VIRTIO_GPU_F_CONTEXT_INIT);
  stage = "feature-negotiation";
  rc = virtio_pci_negotiate_features(&vgpu->vpci, want);
  if (rc < 0)
    goto fail;

  /* Allocate single MSI-X vector for ctrlq + config change */
  stage = "msix-enable";
  int nvectors = pci_enable_msix(pdev, 1);
  if (nvectors <= 0) {
    rc = nvectors < 0 ? nvectors : -ENOSPC;
    goto fail;
  }
  vgpu->vpci.msix_vector = pdev->msix_vector_base;
  printk(LOG_INFO, "virtio_gpu: MSI-X vector %d\n", vgpu->vpci.msix_vector);

  /* Initialize ctrlq */
  stage = "ctrlq-init";
  rc = virtio_gpu_init_ctrlq(vgpu);
  if (rc < 0)
    goto fail;

  /* Wire up vring completion callback: vring_poll_used calls this for each
     completed descriptor, setting the per-command completed flag so each
     sleeping caller can independently detect its own response. */
  vgpu->ctrlq.callback = virtio_gpu_cmd_callback;

  /* Register ISR */
  stage = "irq-registration";
  rc = pci_request_irq(pdev, 0, virtio_gpu_isr);
  if (rc < 0)
    goto fail;
  pci_msix_unmask_entry(pdev, 0);

  /* Set DRIVER_OK status (preserve FEATURES_OK negotiated earlier) */
  virtio_pci_write_status(
      &vgpu->vpci, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                       VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);
  __atomic_store_n(&backend->hardware_live, true, __ATOMIC_RELEASE);
  __atomic_store_n(&backend->accepting_commands, true, __ATOMIC_RELEASE);

  /* Read device config (num_scanouts) */
  virtio_pci_read_dev_cfg(&vgpu->vpci, 0, &vgpu->config, sizeof(vgpu->config));
  printk(LOG_INFO, "virtio_gpu: num_scanouts=%u num_capsets=%u\n",
         vgpu->config.num_scanouts, vgpu->config.num_capsets);

  /* Initialize DRM device state */
  stage = "drm-device-allocation";
  __memset(&virtio_gpu_backend->drm, 0, sizeof(virtio_gpu_backend->drm));
  virtio_gpu_backend->drm.initialized = true;
  if (!drm_dev_alloc()) {
    rc = -ENOMEM;
    goto fail;
  }
  stage = "drm-object-allocation";
  virtio_gpu_backend->drm.crtc_id =
      drm_core_object_create(virtio_gpu_backend->core, DRM_MODE_OBJECT_CRTC);
  virtio_gpu_backend->drm.connector_id = drm_core_object_create(
      virtio_gpu_backend->core, DRM_MODE_OBJECT_CONNECTOR);
  virtio_gpu_backend->drm.encoder_id =
      drm_core_object_create(virtio_gpu_backend->core, DRM_MODE_OBJECT_ENCODER);
  virtio_gpu_backend->drm.plane_id =
      drm_core_object_create(virtio_gpu_backend->core, DRM_MODE_OBJECT_PLANE);
  if (!virtio_gpu_backend->drm.crtc_id ||
      !virtio_gpu_backend->drm.connector_id ||
      !virtio_gpu_backend->drm.encoder_id ||
      !virtio_gpu_backend->drm.plane_id) {
    rc = -ENOSPC;
    goto fail;
  }
  virtio_gpu_backend->drm.dumb_lock = SPINLOCK_INIT;
  virtio_gpu_backend->drm.fb_lock = SPINLOCK_INIT;
  virtio_gpu_backend->drm.ctx_id_lock = SPINLOCK_INIT;
  virtio_gpu_backend->drm.fence_lock = SPINLOCK_INIT;
  virtio_gpu_backend->drm.virgl_lock = SPINLOCK_INIT;
  virtio_gpu_backend->drm.next_virgl_handle = VIRGL_HANDLE_BASE;
  virtio_gpu_backend->drm.next_dumb_handle = 1;
  virtio_gpu_backend->drm.next_fb_id = 1;
  /* Query capsets after virtio_gpu_backend->drm is zeroed/initialized. */
  drm_query_capsets(vgpu);

  /* Default display mode (runtime-overridable; see
     virtio_gpu_backend->drm.fb_*). Change DRM_FB_WIDTH/HEIGHT to alter the
     default. */
  virtio_gpu_backend->drm.fb_width = DRM_FB_WIDTH;
  virtio_gpu_backend->drm.fb_height = DRM_FB_HEIGHT;
  virtio_gpu_backend->drm.fb_bpp = DRM_FB_BPP;
  virtio_gpu_backend->drm.fb_pitch =
      virtio_gpu_backend->drm.fb_width * (virtio_gpu_backend->drm.fb_bpp / 8);

  /* ===== Property infrastructure initialization (Phase C) ===== */
  /* Create properties */
  uint32_t p_src_x = drm_core_property_create_range(
      virtio_gpu_backend->core, "SRC_X", 0, 0xFFFFFFFF, true);
  uint32_t p_src_y = drm_core_property_create_range(
      virtio_gpu_backend->core, "SRC_Y", 0, 0xFFFFFFFF, true);
  uint32_t p_src_w = drm_core_property_create_range(
      virtio_gpu_backend->core, "SRC_W", 0, 0xFFFFFFFF, true);
  uint32_t p_src_h = drm_core_property_create_range(
      virtio_gpu_backend->core, "SRC_H", 0, 0xFFFFFFFF, true);
  uint32_t p_active = drm_core_property_create_range(virtio_gpu_backend->core,
                                                     "ACTIVE", 0, 1, false);

  const uint64_t dpms_vals[4] = {0, 1, 2, 3};
  const char *dpms_names[4] = {"On", "Standby", "Suspend", "Off"};
  uint32_t p_dpms = drm_core_property_create_enum(
      virtio_gpu_backend->core, "DPMS", dpms_vals, dpms_names, 4, false);

  uint32_t p_edid =
      drm_core_property_create_blob(virtio_gpu_backend->core, "EDID", true);
  uint32_t p_in_formats = drm_core_property_create_blob(
      virtio_gpu_backend->core, "IN_FORMATS", true);
  uint32_t p_crtc_id = drm_core_property_create_object(
      virtio_gpu_backend->core, "CRTC_ID", DRM_MODE_OBJECT_CRTC, false);
  uint32_t p_fb_id = drm_core_property_create_object(
      virtio_gpu_backend->core, "FB_ID", DRM_MODE_OBJECT_FB, false);
  uint32_t p_mode_id =
      drm_core_property_create_blob(virtio_gpu_backend->core, "MODE_ID", false);
  const uint64_t plane_type_vals[3] = {
      DRM_PLANE_TYPE_OVERLAY, DRM_PLANE_TYPE_PRIMARY, DRM_PLANE_TYPE_CURSOR};
  const char *plane_type_names[3] = {"Overlay", "Primary", "Cursor"};
  uint32_t p_plane_type =
      drm_core_property_create_enum(virtio_gpu_backend->core, "type",
                                    plane_type_vals, plane_type_names, 3, true);
  if (!p_src_x || !p_src_y || !p_src_w || !p_src_h || !p_active || !p_dpms ||
      !p_edid || !p_in_formats || !p_crtc_id || !p_fb_id || !p_mode_id ||
      !p_plane_type) {
    stage = "drm-property-allocation";
    rc = -ENOSPC;
    goto fail;
  }

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
  if (!in_fmts_blob_data) {
    stage = "drm-format-blob-buffer";
    rc = -ENOMEM;
    goto fail;
  }
  struct drm_format_modifier_blob *hdr =
      (struct drm_format_modifier_blob *)in_fmts_blob_data;
  __memset(hdr, 0, sizeof(*hdr));
  hdr->version = 1;
  hdr->count_formats = 4;
  hdr->formats_offset = sizeof(struct drm_format_modifier_blob);
  hdr->count_modifiers = 1;
  hdr->modifiers_offset = (uint32_t)(sizeof(struct drm_format_modifier_blob) +
                                     4 * sizeof(uint32_t));
  __memcpy(in_fmts_blob_data + sizeof(struct drm_format_modifier_blob), in_fmts,
           4 * sizeof(uint32_t));
  __memcpy(in_fmts_blob_data + hdr->modifiers_offset, in_mods,
           sizeof(struct drm_format_modifier));
  uint32_t in_fmts_blob_id = drm_core_blob_create(
      virtio_gpu_backend->core, in_fmts_blob_data, in_fmts_blob_size);
  kfree(in_fmts_blob_data);
  if (!in_fmts_blob_id) {
    stage = "drm-format-blob-allocation";
    rc = -ENOSPC;
    goto fail;
  }

  /* Generate EDID blob */
  uint8_t edid_data[128];
  extern void drm_generate_edid(uint8_t * buf, size_t bufsz, uint32_t width,
                                uint32_t height);
  drm_generate_edid(edid_data, sizeof(edid_data),
                    virtio_gpu_backend->drm.fb_width,
                    virtio_gpu_backend->drm.fb_height);
  uint32_t edid_blob_id =
      drm_core_blob_create(virtio_gpu_backend->core, edid_data, 128);
  if (!edid_blob_id) {
    stage = "drm-edid-blob-allocation";
    rc = -ENOSPC;
    goto fail;
  }

  struct drm_property_binding {
    uint32_t object_id;
    uint32_t object_type;
    uint32_t property_id;
    uint64_t value;
  } bindings[] = {
      {DRM_CONNECTOR_ID, DRM_MODE_OBJECT_CONNECTOR, p_dpms, DRM_MODE_DPMS_ON},
      {DRM_CONNECTOR_ID, DRM_MODE_OBJECT_CONNECTOR, p_edid, edid_blob_id},
      {DRM_PLANE_ID, DRM_MODE_OBJECT_PLANE, p_plane_type,
       DRM_PLANE_TYPE_PRIMARY},
      {DRM_PLANE_ID, DRM_MODE_OBJECT_PLANE, p_in_formats, in_fmts_blob_id},
      {DRM_PLANE_ID, DRM_MODE_OBJECT_PLANE, p_crtc_id, 0},
      {DRM_PLANE_ID, DRM_MODE_OBJECT_PLANE, p_fb_id, 0},
      {DRM_PLANE_ID, DRM_MODE_OBJECT_PLANE, p_src_x, 0},
      {DRM_PLANE_ID, DRM_MODE_OBJECT_PLANE, p_src_y, 0},
      {DRM_PLANE_ID, DRM_MODE_OBJECT_PLANE, p_src_w, 0},
      {DRM_PLANE_ID, DRM_MODE_OBJECT_PLANE, p_src_h, 0},
      {DRM_CRTC_ID, DRM_MODE_OBJECT_CRTC, p_active, 0},
      {DRM_CRTC_ID, DRM_MODE_OBJECT_CRTC, p_mode_id, 0},
  };
  stage = "drm-property-binding";
  for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++) {
    rc = drm_core_object_add_property(
        virtio_gpu_backend->core, bindings[i].object_id,
        bindings[i].object_type, bindings[i].property_id, bindings[i].value);
    if (rc < 0)
      goto fail;
  }

  stage = "event-workqueue-allocation";
  init_work(&virtio_gpu_backend->event_work, virtio_gpu_event_work);
  virtio_gpu_backend->event_wq =
      alloc_ordered_workqueue("virtio-gpu-event", NULL, NULL);
  if (!virtio_gpu_backend->event_wq) {
    rc = -ENOMEM;
    virtio_gpu_backend->drm.initialized = false;
    goto fail;
  }

  /* Publish minors only after every hardware and software dependency exists. */
  stage = "drm-device-registration";
  rc = drm_dev_register();
  if (rc < 0)
    goto fail;

  printk(LOG_INFO, "virtio_gpu: init done\n");
  return 0;

fail:
  printk(LOG_ERROR, "virtio_gpu: probe failed stage=%s rc=%d\n", stage, rc);
  virtio_gpu_remove(pdev);
  return rc;
}

static void virtio_gpu_drop_pending(struct virtio_gpu_backend *backend) {
  uint64_t flags;
  spin_lock_irqsave(&backend->vgpu.pending_lock, &flags);
  struct virtgpu_cmd_pending *pending = backend->vgpu.pending_list;
  backend->vgpu.pending_list = NULL;
  spin_unlock_irqrestore(&backend->vgpu.pending_lock, flags);
  while (pending) {
    struct virtgpu_cmd_pending *next = pending->next;
    if (pending->hdr.flags & VIRTIO_GPU_FLAG_FENCE) {
      struct drm_fence *fence = drm_fence_find(
          pending->hdr.ctx_id, pending->hdr.ring_idx, pending->hdr.fence_id);
      virtio_drm_fence_signal(fence, pending->hdr.ctx_id, pending->hdr.ring_idx,
                              pending->hdr.fence_id);
      drm_fence_put(fence);
    }
    if (pending->waiter)
      wake_wq_target(pending->waiter);
    kfree(pending->cmd_buf);
    kfree(pending->resp_buf);
    kfree(pending);
    pending = next;
  }
}

static void virtio_gpu_backend_release(void *driver_private) {
  struct virtio_gpu_backend *backend = driver_private;
  if (!backend)
    return;
  for (uint32_t i = 0; i < backend->drm.num_capsets; i++)
    kfree(backend->drm.capsets[i].data);
  kfree(backend->files);
  if (virtio_gpu_backend == backend)
    virtio_gpu_backend = NULL;
  kfree(backend);
}

static void virtio_gpu_remove(pci_device *pdev) {
  struct virtio_gpu_backend *backend = pci_get_driver_private(pdev);
  if (!backend)
    return;

  printk(LOG_INFO, "virtio_gpu: controlled stop begin\n");
  __atomic_store_n(&backend->accepting_commands, false, __ATOMIC_RELEASE);
  backend->drm.initialized = false;
  if (backend->core &&
      drm_core_device_state(backend->core) == DRM_CORE_REGISTERED)
    drm_core_device_unregister(backend->core);

  if (backend->event_wq) {
    cancel_work_sync(&backend->event_work);
    destroy_workqueue(backend->event_wq);
    backend->event_wq = NULL;
  }

  /* Driver-owned work has drained, so no waiter still needs queue IRQs. */
  __atomic_store_n(&backend->hardware_live, false, __ATOMIC_RELEASE);
  pci_disable_interrupts(pdev);
  if (backend->vgpu.vpci.common && pdev->enabled)
    virtio_pci_write_status(&backend->vgpu.vpci, 0);
  virtio_gpu_drop_pending(backend);
  if (backend->vgpu.ctrlq.desc)
    vring_destroy(&backend->vgpu.ctrlq);
  pci_disable_device(pdev);
  pci_set_driver_private(pdev, NULL);

  struct drm_core_device *core = backend->core;
  backend->core = NULL;
  if (core)
    drm_core_device_put(core);
  else
    virtio_gpu_backend_release(backend);
  printk(LOG_INFO, "virtio_gpu: controlled stop done\n");
}

static const struct pci_device_id virtio_gpu_pci_ids[] = {
    {.vendor = VIRTIO_PCI_VENDOR_ID,
     .device = VIRTIO_PCI_DEVICE_ID,
     .subsystem_vendor = PCI_ANY_ID,
     .subsystem_device = PCI_ANY_ID},
    {0},
};

static const struct pci_driver virtio_gpu_pci_driver = {
    .name = "virtio_gpu",
    .id_table = virtio_gpu_pci_ids,
    .probe = virtio_gpu_probe,
    .remove = virtio_gpu_remove,
};

int virtio_gpu_register_driver(void) {
  return pci_register_driver(&virtio_gpu_pci_driver);
}

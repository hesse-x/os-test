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
#include "kernel/bsd/sysfs.h"
#include "kernel/driver/driver.h"
#include "kernel/driver/drm_internal.h"
#include "kernel/driver/pci.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/mem/kasan.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/mm_types.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/trap.h"
#include "kernel/xcore/wait_queue.h"
#include "kernel/xcore/xtask.h"

#include <xos/errno.h>
#include <xos/page.h>
#include <xos/socket.h>

#include "drm/drm.h"
#include "drm/drm_fourcc.h"
#include "drm/drm_mode.h"

struct virtio_gpu_device g_virtio_gpu;
struct drm_device g_drm;
struct drm_property g_drm_properties[DRM_MAX_PROPERTIES];
int g_drm_next_prop_id = 1;
struct drm_blob g_drm_blobs[DRM_MAX_BLOBS];
int g_drm_next_blob_id = 1;
spinlock g_drm_files_lock = SPINLOCK_INIT;
struct drm_file g_drm_files[MAX_DRM_FDS];
struct drm_cursor g_drm_cursor;

/* Per-command completion context: stack-allocated in virtio_gpu_send_cmd.
   The vring callback sets completed=true when the device processes this
   command's descriptor, allowing each caller to independently detect its
   own completion without a shared global flag. */
struct virtio_gpu_cmd_ctx {
  volatile bool completed; /* set by virtio_gpu_cmd_callback from ISR */
};

static void virtio_gpu_cmd_callback(void *ctx, uint32_t len) {
  /* Defensive: ctx must never be NULL in normal operation (it is the
     per-cmd context set in vring_add_buf).  A NULL here means the used
     ring was drained against a descriptor whose ctx had already been
     cleared/reused by a concurrent drain — historically the direct cause
     of the NULL-write #PF.  Harmless-ize it rather than crashing. */
  if (!ctx)
    return;
  struct virtio_gpu_cmd_ctx *cmd_ctx = (struct virtio_gpu_cmd_ctx *)ctx;
  cmd_ctx->completed = true;
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
       __wake_up is performed after releasing cmd_lock to keep the lock
       order one-directional (cmd_wq.lock → scheduler_lock inside
       wake_with_event) and to minimize time spent in interrupt context. */
    uint64_t flags;
    spin_lock_irqsave(&vgpu->cmd_lock, &flags);
    vring_poll_used(&vgpu->ctrlq);
    spin_unlock_irqrestore(&vgpu->cmd_lock, flags);
    __wake_up(&vgpu->cmd_wq, 0);
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
  struct virtio_gpu_cmd_ctx cmd_ctx = {.completed = false};

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
  static const char driver_name[] = "drm";
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
    c->value = 0;
    return 0;
  case 0x0D:      /* DRM_CAP_ATOMIC */
    c->value = 0; /* force legacy path */
    return 0;
  case DRM_CAP_TIMESTAMP_MONOTONIC:
    c->value = 0;
    return 0;
  case DRM_CAP_ASYNC_PAGE_FLIP:
    c->value = 0;
    return 0;
  case 0x10:
    c->value = 0;
    return 0; /* DRM_CAP_ADDFB2_MODIFIERS */
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
static struct drm_file *drm_file_current(void);

/* DRM_IOCTL_SET_MASTER — per-fd 互斥 */
static long drm_ioctl_set_master(void) {
  struct drm_file *f = drm_file_current();
  if (!f)
    return -EBADF;

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

static long drm_ioctl_drop_master(void) {
  struct drm_file *f = drm_file_current();
  if (!f)
    return -EBADF;

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
static long drm_ioctl_get_magic(void *arg) {
  struct drm_auth *a = (struct drm_auth *)arg;
  if (!a)
    return -EFAULT;
  struct drm_file *f = drm_file_current();
  if (!f)
    return -EBADF;

  a->magic = ++g_drm.magic_counter;
  f->authenticated_magic = a->magic;
  f->auth_valid = false; /* not yet authenticated */
  return 0;
}

/* DRM_IOCTL_AUTH_MAGIC — 严格校验：仅 master fd 可认证，且 magic 必须是已签发的
 */
static long drm_ioctl_auth_magic(void *arg) {
  struct drm_auth *a = (struct drm_auth *)arg;
  if (!a)
    return -EFAULT;
  struct drm_file *current = drm_file_current();
  if (!current)
    return -EBADF;

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
  if (!d)
    return -EINVAL;
  virtio_gpu_set_scanout(0, d->virtio_res_id, 0, 0, g_drm.fb_width,
                         g_drm.fb_height);
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
static long drm_ioctl_create_dumb(void *arg) {
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
  struct drm_file *cf = drm_file_current();
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
static long drm_ioctl_destroy_dumb(void *arg) {
  struct drm_mode_destroy_dumb *d = (struct drm_mode_destroy_dumb *)arg;
  spin_lock(&g_drm.dumb_lock);
  struct drm_dumb_buffer *buf = drm_find_dumb((int)d->handle);
  if (!buf) {
    spin_unlock(&g_drm.dumb_lock);
    return -EINVAL;
  }
  buf->refcount--;
  if (buf->refcount <= 0) {
    uint32_t rid = buf->virtio_res_id;
    __memset(buf, 0, sizeof(*buf));
    spin_unlock(&g_drm.dumb_lock);
    virtio_gpu_resource_unref(rid);
    return 0;
  }
  spin_unlock(&g_drm.dumb_lock);
  return 0;
}

/* DRM_IOCTL_GEM_CLOSE
 * Called by Mesa after ADDFB2 to release the handle reference.
 * In this simplified model, same semantics as DESTROY_DUMB. */
static long drm_ioctl_gem_close(void *arg) {
  struct drm_gem_close *c = (struct drm_gem_close *)arg;
  if (!c)
    return -EFAULT;

  if (c->handle == 0 || c->handle > MAX_DUMB_BUFFERS)
    return -ENOENT;

  spin_lock(&g_drm.dumb_lock);
  struct drm_dumb_buffer *buf = drm_find_dumb((int)c->handle);
  if (!buf) {
    spin_unlock(&g_drm.dumb_lock);
    return -ENOENT;
  }
  buf->refcount--;
  if (buf->refcount <= 0) {
    uint32_t rid = buf->virtio_res_id;
    __memset(buf, 0, sizeof(*buf));
    spin_unlock(&g_drm.dumb_lock);
    virtio_gpu_resource_unref(rid);
    return 0;
  }
  spin_unlock(&g_drm.dumb_lock);
  return 0;
}

/* DRM_IOCTL_MODE_ADDFB */
static long drm_ioctl_addfb(void *arg) {
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
  struct drm_file *cf = drm_file_current();
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
    return 32;
  case DRM_FORMAT_RGB565:
    return 16;
  default:
    return 0;
  }
}

/* DRM_IOCTL_MODE_ADDFB2 */
static long drm_ioctl_addfb2(void *arg) {
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

  /* Validate handle */
  if (c->handles[0] == 0 || c->handles[0] > MAX_DUMB_BUFFERS)
    return -ENOENT;

  spin_lock(&g_drm.dumb_lock);
  struct drm_dumb_buffer *d = drm_find_dumb((int)c->handles[0]);
  int found = (d != NULL);
  spin_unlock(&g_drm.dumb_lock);
  if (!found)
    return -ENOENT;

  /* Allocate fb_id (shared with ADDFB) */
  spin_lock(&g_drm.fb_lock);
  int fb_id = drm_alloc_fb_id();
  if (fb_id < 0) {
    spin_unlock(&g_drm.fb_lock);
    return -ENOMEM;
  }
  struct drm_framebuffer *fb = &g_drm.fbs[fb_id - 1];
  spin_unlock(&g_drm.fb_lock);

  fb->dumb_handle = (int)c->handles[0];
  fb->width = c->width;
  fb->height = c->height;
  fb->pitch = c->pitches[0];
  fb->bpp = (uint32_t)bpp;

  /* Bump dumb buffer refcount */
  spin_lock(&g_drm.dumb_lock);
  d = drm_find_dumb((int)c->handles[0]);
  if (d)
    d->refcount++;
  spin_unlock(&g_drm.dumb_lock);

  c->fb_id = (uint32_t)fb_id;

  /* Track in per-fd list (Phase C) */
  {
    struct drm_file *cf = drm_file_current();
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

  spin_lock(&g_drm.dumb_lock);
  struct drm_dumb_buffer *d = drm_find_dumb(dumb_handle);
  if (d)
    d->refcount--;
  spin_unlock(&g_drm.dumb_lock);
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

  switch (c->flags & DRM_MODE_CURSOR_FLAGS) {
  case DRM_MODE_CURSOR_BO: {
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

/* DRM_IOCTL_MODE_PAGE_FLIP */
static long drm_ioctl_page_flip(void *arg) {
  struct drm_mode_crtc_page_flip *p = (struct drm_mode_crtc_page_flip *)arg;
  if (p->crtc_id != DRM_CRTC_ID)
    return -EINVAL;
  struct drm_framebuffer *fb = drm_find_fb((int)p->fb_id);
  if (!fb)
    return -EINVAL;
  struct drm_dumb_buffer *d = drm_find_dumb(fb->dumb_handle);
  if (!d)
    return -EINVAL;

  drm_cursor_overlay(d); /* Phase C: overlay cursor before transfer */

  virtio_gpu_transfer_2d(d->virtio_res_id, 0, 0, d->width, d->height, 0);
  virtio_gpu_flush(d->virtio_res_id, 0, 0, d->width, d->height);

  g_drm.current_fb_id = p->fb_id;

  if (p->flags & DRM_MODE_PAGE_FLIP_EVENT) {
    spin_lock(&g_drm.event_lock);
    g_drm.event_pending = true;
    g_drm.event_sequence++;
    g_drm.event_user_data = p->user_data;
    spin_unlock(&g_drm.event_lock);
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
  if (!d)
    return -EINVAL;
  drm_cursor_overlay(d); /* Phase C: overlay cursor before transfer */

  virtio_gpu_transfer_2d(d->virtio_res_id, 0, 0, d->width, d->height, 0);
  virtio_gpu_flush(d->virtio_res_id, 0, 0, d->width, d->height);
  return 0;
}

/* Main DRM ioctl dispatcher */
long drm_ioctl(uint32_t cmd, void *arg) {
  printk(LOG_DEBUG, "drm_ioctl: cmd=0x%x initialized=%d\n", cmd,
         g_drm.initialized);
  if (!g_drm.initialized)
    return -ENODEV;
  switch (cmd) {
  case DRM_IOCTL_VERSION:
    return drm_ioctl_version(arg);
  case DRM_IOCTL_GET_CAP:
    return drm_ioctl_get_cap(arg);
  case DRM_IOCTL_SET_CLIENT_CAP:
    return drm_ioctl_set_client_cap(arg);
  case DRM_IOCTL_SET_MASTER:
    return drm_ioctl_set_master();
  case DRM_IOCTL_DROP_MASTER:
    return drm_ioctl_drop_master();
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
    return drm_ioctl_create_dumb(arg);
  case DRM_IOCTL_MODE_MAP_DUMB:
    return drm_ioctl_map_dumb(arg);
  case DRM_IOCTL_MODE_DESTROY_DUMB:
    return drm_ioctl_destroy_dumb(arg);
  case DRM_IOCTL_MODE_ADDFB:
    return drm_ioctl_addfb(arg);
  case DRM_IOCTL_MODE_ADDFB2:
    return drm_ioctl_addfb2(arg);
  case DRM_IOCTL_MODE_RMFB:
    return drm_ioctl_rmfb(arg);
  case DRM_IOCTL_MODE_PAGE_FLIP:
    return drm_ioctl_page_flip(arg);
  case DRM_IOCTL_MODE_DIRTYFB:
    return drm_ioctl_dirtyfb(arg);
  case DRM_IOCTL_MODE_CURSOR2:
    return drm_ioctl_cursor2(arg);
  case DRM_IOCTL_MODE_GETFB:
    return drm_ioctl_getfb(arg);
  case DRM_IOCTL_GET_MAGIC:
    return drm_ioctl_get_magic(arg);
  case DRM_IOCTL_AUTH_MAGIC:
    return drm_ioctl_auth_magic(arg);
  case DRM_IOCTL_GEM_CLOSE:
    return drm_ioctl_gem_close(arg);
  case DRM_IOCTL_MODE_GETPROPERTY:
    return drm_ioctl_getproperty(arg);
  case DRM_IOCTL_MODE_SETPROPERTY:
  case DRM_IOCTL_MODE_OBJ_SETPROPERTY:
    return -ENOSYS;
  case DRM_IOCTL_MODE_GETPROPBLOB:
    return drm_ioctl_getpropblob(arg);
  case DRM_IOCTL_MODE_OBJ_GETPROPERTIES:
    return drm_ioctl_obj_getproperties(arg);
  case DRM_IOCTL_PRIME_HANDLE_TO_FD:
  case DRM_IOCTL_PRIME_FD_TO_HANDLE:
    return -ENOSYS;
  default:
    printk(LOG_WARN, "drm_ioctl: unknown cmd 0x%x\n", cmd);
    return -ENOSYS;
  }
}

/* ===== per-fd lookup (Phase C) ===== */
static struct drm_file *drm_file_current(void) {
  /* Find the drm_file slot for the current task.
   * Called from ioctl handlers; the current task is what's running.
   * Iterate g_drm_files[] matching ->proc == current xtask. */
  xtask *cur = current_task;
  spin_lock(&g_drm_files_lock);
  for (int i = 0; i < MAX_DRM_FDS; i++) {
    if (g_drm_files[i].used && g_drm_files[i].proc == cur) {
      spin_unlock(&g_drm_files_lock);
      return &g_drm_files[i];
    }
  }
  spin_unlock(&g_drm_files_lock);
  return NULL;
}

/* ===== DRM device ops ===== */
int drm_open(xtask *proc, int fd) {
  spin_lock(&g_drm_files_lock);
  for (int i = 0; i < MAX_DRM_FDS; i++) {
    if (!g_drm_files[i].used) {
      g_drm_files[i].used = true;
      g_drm_files[i].fd = fd;
      g_drm_files[i].proc = proc;
      g_drm_files[i].is_master = false;
      g_drm_files[i].authenticated_magic = 0;
      g_drm_files[i].auth_valid = false;
      spin_unlock(&g_drm_files_lock);
      return 0;
    }
  }
  spin_unlock(&g_drm_files_lock);
  return -ENFILE;
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

  /* Release dumb buffer reference */
  if (dumb_handle > 0) {
    spin_lock(&g_drm.dumb_lock);
    struct drm_dumb_buffer *d = drm_find_dumb(dumb_handle);
    if (d) {
      d->refcount--;
      if (d->refcount <= 0) {
        uint32_t rid = d->virtio_res_id;
        __memset(d, 0, sizeof(*d));
        spin_unlock(&g_drm.dumb_lock);
        virtio_gpu_resource_unref(rid);
      } else {
        spin_unlock(&g_drm.dumb_lock);
      }
    } else {
      spin_unlock(&g_drm.dumb_lock);
    }
  }
}

/* Helper: release a dumb buffer (force release) */
static void drm_release_dumb(int handle) {
  spin_lock(&g_drm.dumb_lock);
  struct drm_dumb_buffer *d = drm_find_dumb(handle);
  if (d) {
    uint32_t rid = d->virtio_res_id;
    __memset(d, 0, sizeof(*d));
    spin_unlock(&g_drm.dumb_lock);
    virtio_gpu_resource_unref(rid);
  } else {
    spin_unlock(&g_drm.dumb_lock);
  }
}

int drm_close(xtask *proc, int fd) {
  spin_lock(&g_drm_files_lock);
  for (int i = 0; i < MAX_DRM_FDS; i++) {
    struct drm_file *f = &g_drm_files[i];
    if (!f->used)
      continue;
    if (fd >= 0) {
      /* Normal close via sys_close: match specific fd + proc. */
      if (f->fd != fd || f->proc != proc)
        continue;
    } else {
      /* Process-exit cleanup (file_put → ops->close(proc, -1)):
       * no fd number available, match by proc only.  Each FD_DEV fd
       * gets its own file_put call, so one iteration per entry. */
      if (f->proc != proc)
        continue;
    }
    /* Release per-fd resources (Phase C) */
    for (int j = 0; j < f->created_fb_count; j++) {
      drm_release_fb(f->created_fb_ids[j]);
    }
    for (int j = 0; j < f->created_dumb_count; j++) {
      drm_release_dumb(f->created_dumb_handles[j]);
    }

    if (f->is_master) {
      g_drm.is_master = false;
      drm_master_cleanup();
    }
    __memset(f, 0, sizeof(*f));
    spin_unlock(&g_drm_files_lock);
    return 0;
  }
  spin_unlock(&g_drm_files_lock);
  return 0; /* ignore close on unknown fd */
}

/* forward declarations for ops callbacks defined further below */
static ssize_t drm_read(xtask *proc, int fd, void *buf, size_t count);

static struct dev_ops drm_dev_ops = {
    .driver_pid = 0,
    .is_block = false,
    .open = drm_open,
    .close = drm_close,
    .ioctl = drm_ioctl,
    .mmap = drm_mmap_handler,
    .read = drm_read,
    .poll = drm_poll,
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
  return snprintf(buf, len, "0x%06X\n", pdev->class_code);
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
  printk(LOG_INFO, "drm: registered /dev/dri/card0\n");
}

/* ===== DRM mmap handler =====
   mmap(fd, offset) where offset = handle << 12 (from MAP_DUMB).
   For Phase 3 (single active dumb buffer at a time) we locate the buffer
   by matching size. Map its physical pages into user space. */
__attribute__((no_sanitize("kernel-address"))) uint64_t
drm_mmap_handler(xtask *proc, uint64_t size) {
  spin_lock(&g_drm.dumb_lock);
  struct drm_dumb_buffer *target = NULL;
  for (int i = 0; i < MAX_DUMB_BUFFERS; i++) {
    if (g_drm.dumbs[i].handle != 0 && g_drm.dumbs[i].size == size) {
      target = &g_drm.dumbs[i];
      break;
    }
  }
  spin_unlock(&g_drm.dumb_lock);
  if (!target) {
    printk(LOG_ERROR, "drm_mmap: no dumb buffer of size %llu\n",
           (unsigned long long)size);
    return 0;
  }

  size_t npages = (target->size + PAGE_SIZE - 1) / PAGE_SIZE;
  uint64_t *pml4 =
      (__force uint64_t *)phys_to_virt((__force phys_addr_t)proc->mm->cr3);
  uint64_t vaddr = proc->mm->mmap_brk;
  uint64_t pte_flags = PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX;

  for (size_t i = 0; i < npages; i++) {
    uint64_t page_phys = target->guest_phys + i * PAGE_SIZE;
    if (!map_user_page_direct(pml4, vaddr + i * PAGE_SIZE, page_phys,
                              pte_flags)) {
      for (size_t j = 0; j < i; j++)
        unmap_user_pages(pml4, vaddr + j * PAGE_SIZE,
                         vaddr + (j + 1) * PAGE_SIZE, 1);
      return 0;
    }
  }

  mmap_region *region = (mmap_region *)kmalloc(sizeof(mmap_region));
  if (!region) {
    for (size_t i = 0; i < npages; i++)
      unmap_user_pages(pml4, vaddr + i * PAGE_SIZE, vaddr + (i + 1) * PAGE_SIZE,
                       1);
    return 0;
  }
  region->vaddr = vaddr;
  region->size = npages * PAGE_SIZE;
  region->phys = target->guest_phys;
  region->shm_obj = NULL;
  region->next = proc->mm->mmap_regions;
  proc->mm->mmap_regions = region;
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
    return -EAGAIN;
  }
  if (count < sizeof(struct drm_event_vblank)) {
    spin_unlock(&g_drm.event_lock);
    return -EINVAL;
  }
  struct drm_event_vblank ev;
  __memset(&ev, 0, sizeof(ev));
  ev.base.type = DRM_EVENT_VBLANK;
  ev.base.length = sizeof(ev);
  ev.user_data = g_drm.event_user_data;
  ev.sequence = g_drm.event_sequence;
  g_drm.event_pending = false;
  spin_unlock(&g_drm.event_lock);

  size_t cr = copy_to_user(buf, &ev, sizeof(ev));
  if (cr != 0)
    return -EFAULT;
  return (ssize_t)sizeof(ev);
}

void virtio_gpu_init(void) {
  struct virtio_gpu_device *vgpu = &g_virtio_gpu;
  __memset(vgpu, 0, sizeof(*vgpu));
  vgpu->cmd_lock = SPINLOCK_INIT;
  init_wait_queue_head(&vgpu->cmd_wq);

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

  /* Negotiate features: only VIRTIO_F_VERSION_1 */
  if (virtio_pci_negotiate_features(&vgpu->vpci, 1ULL << VIRTIO_F_VERSION_1) <
      0) {
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
  g_drm.next_dumb_handle = 1;
  g_drm.next_fb_id = 1;

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

  /* Plane(4): IN_FORMATS + CRTC_ID + FB_ID + SRC_X/Y/W/H */
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

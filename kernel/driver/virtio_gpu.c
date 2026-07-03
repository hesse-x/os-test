#include "kernel/driver/virtio_gpu.h"
#include "arch/x64/apic.h"
#include "arch/x64/paging.h"
#include "kernel/driver/driver.h"
#include "kernel/driver/pci.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/trap.h"

struct virtio_gpu_device g_virtio_gpu;

/* Forward declarations */
static void virtio_gpu_isr(trapframe *tf);
static int virtio_gpu_send_cmd(struct virtio_gpu_device *vgpu, void *cmd_buf,
                               size_t cmd_len, void *resp_buf, size_t resp_len);

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

  /* Assign MSI-X vector to this queue (set before enable) */
  common->queue_msix_vector = (uint16_t)vgpu->vpci.msix_vector;

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
    /* Drain used ring: process completed commands */
    int n = vring_poll_used(&vgpu->ctrlq);
    if (n > 0 && vgpu->waiter) {
      /* Wake the task waiting for response */
      xtask *w = vgpu->waiter;
      vgpu->waiter = NULL;
      vgpu->response_ready = true;
      wake_with_event(w, WAIT_RECV);
    }
  }
  /* config change: not handled in Phase 2 (no EDID) */

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
  spin_lock(&vgpu->cmd_lock);

  /* Physical addresses for descriptors (must be guest-physical) */
  uint64_t cmd_phys = (uint64_t)PHY_ADDR((uintptr_t)cmd_buf);
  uint64_t resp_phys = (uint64_t)PHY_ADDR((uintptr_t)resp_buf);

  /* Set up 2 descriptors: cmd (device-readable) + resp (device-writable) */
  uint64_t addrs[2] = {cmd_phys, resp_phys};
  uint32_t lens[2] = {(uint32_t)cmd_len, (uint32_t)resp_len};
  uint16_t flags[2] = {0, VRING_DESC_F_WRITE}; /* cmd: read-only; resp: write */

  vgpu->response_buf = resp_buf;
  vgpu->response_len = resp_len;
  vgpu->response_ready = false;
  vgpu->waiter = current_task; /* NULL during early boot (driver_init) */

  int head = vring_add_buf(&vgpu->ctrlq, addrs, lens, flags, 2, NULL);
  if (head < 0) {
    spin_unlock(&vgpu->cmd_lock);
    printk(LOG_ERROR, "virtio_gpu: vring_add_buf failed\n");
    return -1;
  }

  /* Make command visible + kick */
  vring_kick(&vgpu->ctrlq);
  virtio_pci_notify(&vgpu->vpci, vgpu->ctrlq.notify_off);

  /* During early boot (driver_init, before idle process exists) there is no
     process context to sleep in: current_task is NULL and schedule() cannot
     block. Poll the used ring synchronously instead. Once a process context
     is available, sleep/wake via the ISR as originally intended. */
  if (current_task == NULL) {
    while (!vring_has_used(&vgpu->ctrlq)) {
      __asm__ volatile("pause" ::: "memory");
    }
    vring_poll_used(&vgpu->ctrlq);
    vgpu->response_ready = true;
    spin_unlock(&vgpu->cmd_lock);
    return 0;
  }

  /* Sleep until ISR wakes us */
  current_task->state = BLOCKED;
  current_task->wait_event = WAIT_RECV;
  spin_unlock(&vgpu->cmd_lock);
  schedule();

  /* Woken up: response is in resp_buf */
  return vgpu->response_ready ? 0 : -1;
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

/* ===== 2.E: real init + driver definition ===== */

void virtio_gpu_init(void) {
  struct virtio_gpu_device *vgpu = &g_virtio_gpu;
  __memset(vgpu, 0, sizeof(*vgpu));
  vgpu->cmd_lock = SPINLOCK_INIT;

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

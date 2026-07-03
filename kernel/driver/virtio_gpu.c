#include "kernel/driver/virtio_gpu.h"
#include "kernel/driver/pci.h"
#include "kernel/driver/driver.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/trap.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/mem/kasan.h"
#include "kernel/bsd/devtmpfs.h"
#include "kernel/xcore/mm_types.h"
#include "arch/x64/paging.h"
#include "arch/x64/memlayout.h"
#include "arch/x64/apic.h"
#include "kernel/driver/drm_internal.h"
#include "utils/drm.h"
#include "xos/ioctl.h"
#include "xos/errno.h"
#include "xos/socket.h"

struct virtio_gpu_device g_virtio_gpu;
struct drm_device g_drm;

/* Forward declarations */
static void virtio_gpu_isr(trapframe *tf);
static int virtio_gpu_send_cmd(struct virtio_gpu_device *vgpu,
                               void *cmd_buf, size_t cmd_len,
                               void *resp_buf, size_t resp_len);

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
    printk(LOG_INFO, "virtio_gpu: ctrlq size=%u notify_off=%u\n", size, notify_off);

    /* Allocate and initialize the virtqueue */
    if (vring_create(&vgpu->ctrlq, VIRTIO_GPU_CTRLQ_INDEX, size, notify_off) < 0) {
        printk(LOG_ERROR, "virtio_gpu: vring_create failed\n");
        return -1;
    }

    /* Program queue addresses into common config */
    common->queue_desc_lo   = (uint32_t)(vgpu->ctrlq.desc_phys & 0xFFFFFFFF);
    common->queue_desc_hi   = (uint32_t)(vgpu->ctrlq.desc_phys >> 32);
    common->queue_avail_lo  = (uint32_t)(vgpu->ctrlq.avail_phys & 0xFFFFFFFF);
    common->queue_avail_hi  = (uint32_t)(vgpu->ctrlq.avail_phys >> 32);
    common->queue_used_lo   = (uint32_t)(vgpu->ctrlq.used_phys & 0xFFFFFFFF);
    common->queue_used_hi   = (uint32_t)(vgpu->ctrlq.used_phys >> 32);

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
    printk(LOG_INFO, "virtio_gpu_isr: isr_status=0x%x\n", isr_status);

    if (isr_status & VIRTIO_ISR_QUEUE_INTR) {
        /* Drain used ring: process completed commands */
        int n = vring_poll_used(&vgpu->ctrlq);
        printk(LOG_INFO, "virtio_gpu_isr: poll_used n=%d waiter=%p\n", n, vgpu->waiter);
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
static int virtio_gpu_send_cmd(struct virtio_gpu_device *vgpu,
                               void *cmd_buf, size_t cmd_len,
                               void *resp_buf, size_t resp_len) {
    spin_lock(&vgpu->cmd_lock);

    /* Physical addresses for descriptors (must be guest-physical) */
    uint64_t cmd_phys = (uint64_t)PHY_ADDR((uintptr_t)cmd_buf);
    uint64_t resp_phys = (uint64_t)PHY_ADDR((uintptr_t)resp_buf);

    /* Set up 2 descriptors: cmd (device-readable) + resp (device-writable) */
    uint64_t addrs[2] = { cmd_phys, resp_phys };
    uint32_t lens[2] = { (uint32_t)cmd_len, (uint32_t)resp_len };
    uint16_t flags[2] = { 0, VRING_DESC_F_WRITE };  /* cmd: read-only; resp: write */

    vgpu->response_buf = resp_buf;
    vgpu->response_len = resp_len;
    vgpu->response_ready = false;
    vgpu->waiter = current_task;   /* NULL during early boot (driver_init) */

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

    /* Sleep until ISR wakes us.
       Fallback: MSI-X delivery for virtio-gpu in this environment is unreliable,
       so busy-wait with a bounded timeout before falling through to schedule(). */
    {
        uint64_t spins = 0;
        while (!vring_has_used(&vgpu->ctrlq) && spins < 100000000ULL) {
            __asm__ volatile("pause" ::: "memory");
            spins++;
        }
        if (vring_has_used(&vgpu->ctrlq)) {
            vring_poll_used(&vgpu->ctrlq);
            vgpu->response_ready = true;
            spin_unlock(&vgpu->cmd_lock);
            printk(LOG_INFO, "virtio_gpu_send_cmd: completed via busy poll\n");
            return 0;
        }
    }
    current_task->state = BLOCKED;
    current_task->wait_event = WAIT_RECV;
    spin_unlock(&vgpu->cmd_lock);
    schedule();
    printk(LOG_INFO, "virtio_gpu_send_cmd: woken, response_ready=%d\n", vgpu->response_ready);

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

    int rc = virtio_gpu_send_cmd(&g_virtio_gpu, &cmd, sizeof(cmd), &resp, sizeof(resp));
    if (rc < 0) return rc;
    if (resp.hdr.type != VIRTIO_GPU_RESP_OK_NODATA) {
        printk(LOG_ERROR, "virtio_gpu: CREATE_2D failed, resp type=0x%x\n", resp.hdr.type);
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

    int rc = virtio_gpu_send_cmd(&g_virtio_gpu, &buf, sizeof(buf), &resp, sizeof(resp));
    if (rc < 0) return rc;
    if (resp.hdr.type != VIRTIO_GPU_RESP_OK_NODATA) {
        printk(LOG_ERROR, "virtio_gpu: ATTACH_BACKING failed, resp type=0x%x\n", resp.hdr.type);
        return -1;
    }
    return 0;
}

int virtio_gpu_set_scanout(uint32_t scanout_id, uint32_t resource_id,
                           uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    struct virtio_gpu_set_scanout cmd;
    __memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    cmd.r.x = x; cmd.r.y = y; cmd.r.width = w; cmd.r.height = h;
    cmd.scanout_id = scanout_id;
    cmd.resource_id = resource_id;

    struct virtio_gpu_ctrl_hdr_response resp;
    __memset(&resp, 0, sizeof(resp));

    int rc = virtio_gpu_send_cmd(&g_virtio_gpu, &cmd, sizeof(cmd), &resp, sizeof(resp));
    if (rc < 0) return rc;
    if (resp.hdr.type != VIRTIO_GPU_RESP_OK_NODATA) {
        printk(LOG_ERROR, "virtio_gpu: SET_SCANOUT failed, resp type=0x%x\n", resp.hdr.type);
        return -1;
    }
    return 0;
}

int virtio_gpu_transfer_2d(uint32_t resource_id, uint32_t x, uint32_t y,
                           uint32_t w, uint32_t h, uint64_t offset) {
    struct virtio_gpu_transfer_to_host_2d cmd;
    __memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    cmd.r.x = x; cmd.r.y = y; cmd.r.width = w; cmd.r.height = h;
    cmd.offset = offset;
    cmd.resource_id = resource_id;

    struct virtio_gpu_ctrl_hdr_response resp;
    __memset(&resp, 0, sizeof(resp));

    int rc = virtio_gpu_send_cmd(&g_virtio_gpu, &cmd, sizeof(cmd), &resp, sizeof(resp));
    if (rc < 0) return rc;
    if (resp.hdr.type != VIRTIO_GPU_RESP_OK_NODATA) {
        printk(LOG_ERROR, "virtio_gpu: TRANSFER_TO_HOST_2D failed, resp type=0x%x\n", resp.hdr.type);
        return -1;
    }
    return 0;
}

int virtio_gpu_flush(uint32_t resource_id, uint32_t x, uint32_t y,
                     uint32_t w, uint32_t h) {
    struct virtio_gpu_resource_flush cmd;
    __memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    cmd.r.x = x; cmd.r.y = y; cmd.r.width = w; cmd.r.height = h;
    cmd.resource_id = resource_id;

    struct virtio_gpu_ctrl_hdr_response resp;
    __memset(&resp, 0, sizeof(resp));

    int rc = virtio_gpu_send_cmd(&g_virtio_gpu, &cmd, sizeof(cmd), &resp, sizeof(resp));
    if (rc < 0) return rc;
    if (resp.hdr.type != VIRTIO_GPU_RESP_OK_NODATA) {
        printk(LOG_ERROR, "virtio_gpu: RESOURCE_FLUSH failed, resp type=0x%x\n", resp.hdr.type);
        return -1;
    }
    return 0;
}

/* ===== 2.E: real init + driver definition ===== */

/* ===== DRM ioctl implementation ===== */

static struct drm_dumb_buffer *drm_find_dumb(int handle) {
    if (handle <= 0 || handle > MAX_DUMB_BUFFERS) return NULL;
    struct drm_dumb_buffer *d = &g_drm.dumbs[handle - 1];
    return (d->handle == handle) ? d : NULL;
}

static struct drm_framebuffer *drm_find_fb(int fb_id) {
    if (fb_id <= 0 || fb_id > MAX_FRAMEBUFFERS) return NULL;
    struct drm_framebuffer *fb = &g_drm.fbs[fb_id - 1];
    return (fb->fb_id == fb_id) ? fb : NULL;
}

static int drm_alloc_dumb_handle(void) {
    for (int i = 0; i < MAX_DUMB_BUFFERS; i++) {
        if (g_drm.dumbs[i].handle == 0) {
            g_drm.dumbs[i].handle = i + 1;  /* handle = slot index + 1 */
            g_drm.dumbs[i].refcount = 1;
            return g_drm.dumbs[i].handle;
        }
    }
    return -1;
}

static int drm_alloc_fb_id(void) {
    for (int i = 0; i < MAX_FRAMEBUFFERS; i++) {
        if (g_drm.fbs[i].fb_id == 0) {
            g_drm.fbs[i].fb_id = i + 1;  /* fb_id = slot index + 1 */
            g_drm.fbs[i].refcount = 1;
            return g_drm.fbs[i].fb_id;
        }
    }
    return -1;
}

/* DRM_IOCTL_VERSION */
static long drm_ioctl_version(void *arg) {
    struct drm_version *v = (struct drm_version *)arg;
    v->version_major = 0;
    v->version_minor = 1;
    v->version_patchlevel = 0;
    v->name_len = 0;
    v->date_len = 0;
    v->desc_len = 0;
    return 0;
}

/* DRM_IOCTL_GET_CAP */
static long drm_ioctl_get_cap(void *arg) {
    struct drm_get_cap *c = (struct drm_get_cap *)arg;
    switch (c->capability) {
    case DRM_CAP_DUMB_BUFFER:        c->value = 1; return 0;
    case DRM_CAP_DUMB_PREFERRED_DEPTH: c->value = 24; return 0;
    case DRM_CAP_DUMB_PREFER_SHADOW:   c->value = 0; return 0;
    case DRM_CAP_VBLANK_HIGH_CRTC_TIME: c->value = 0; return 0;
    case DRM_CAP_PRIME:              c->value = 0; return 0;
    case DRM_CAP_TIMESTAMP_MONOTONIC: c->value = 0; return 0;
    case DRM_CAP_ASYNC_PAGE_FLIP:    c->value = 0; return 0;
    default: return -EINVAL;
    }
}

/* DRM_IOCTL_SET_CLIENT_CAP */
static long drm_ioctl_set_client_cap(void *arg) {
    struct drm_set_client_cap *c = (struct drm_set_client_cap *)arg;
    switch (c->capability) {
    case DRM_CLIENT_CAP_UNIVERSAL_PLANES: c->value = 1; return 0;
    case DRM_CLIENT_CAP_ATOMIC:           return -EINVAL;  /* not supported */
    default: return -EINVAL;
    }
}

/* DRM_IOCTL_SET_MASTER / DROP_MASTER */
static long drm_ioctl_set_master(void) {
    g_drm.is_master = true;
    return 0;
}
static long drm_ioctl_drop_master(void) {
    g_drm.is_master = false;
    return 0;
}

/* Fill m->name with "<w>x<h>" (no refresh suffix; buffer is DRM_DISPLAY_INFO_LEN). */
static void drm_mode_fill_name(struct drm_mode_modeinfo *m) {
    uint32_t w = g_drm.fb_width, h = g_drm.fb_height;
    char buf[16];
    int n = 0;
    char tmp[10];
    int i;
    /* width */
    i = 0;
    if (w == 0) tmp[i++] = '0';
    while (w) { tmp[i++] = '0' + (w % 10); w /= 10; }
    while (i) buf[n++] = tmp[--i];
    buf[n++] = 'x';
    /* height */
    i = 0;
    if (h == 0) tmp[i++] = '0';
    while (h) { tmp[i++] = '0' + (h % 10); h /= 10; }
    while (i) buf[n++] = tmp[--i];
    buf[n] = '\0';
    __memset(m->name, 0, sizeof(m->name));
    __memcpy(m->name, buf, n + 1);
}

/* DRM_IOCTL_MODE_GETRESOURCES */
static long drm_ioctl_getresources(void *arg) {
    struct drm_mode_card_res *r = (struct drm_mode_card_res *)arg;
    r->count_crtcs = 1;
    r->count_connectors = 1;
    r->count_encoders = 1;
    r->count_fbs = 0;  /* dynamic */
    r->min_width = g_drm.fb_width;
    r->max_width = g_drm.fb_width;
    r->min_height = g_drm.fb_height;
    r->max_height = g_drm.fb_height;
    return 0;
}

/* DRM_IOCTL_MODE_GETCRTC */
static long drm_ioctl_getcrtc(void *arg) {
    struct drm_mode_crtc *c = (struct drm_mode_crtc *)arg;
    if (c->crtc_id != DRM_CRTC_ID) return -EINVAL;
    c->fb_id = g_drm.current_fb_id;
    c->x = 0; c->y = 0;
    c->mode_valid = g_drm.mode_valid ? 1 : 0;
    if (g_drm.mode_valid) {
        struct drm_mode_modeinfo *m = &c->mode;
        __memset(m, 0, sizeof(*m));
        m->clock = 40000;
        m->hdisplay = g_drm.fb_width; m->hsync_start = g_drm.fb_width + 16;
        m->hsync_end = g_drm.fb_width + 32; m->htotal = g_drm.fb_width + 48;
        m->vdisplay = g_drm.fb_height; m->vsync_start = g_drm.fb_height + 1;
        m->vsync_end = g_drm.fb_height + 4; m->vtotal = g_drm.fb_height + 10;
        m->vrefresh = 60;
        m->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
        drm_mode_fill_name(m);
    }
    return 0;
}

/* DRM_IOCTL_MODE_SETCRTC */
static long drm_ioctl_setcrtc(void *arg) {
    struct drm_mode_crtc *c = (struct drm_mode_crtc *)arg;
    if (c->crtc_id != DRM_CRTC_ID) return -EINVAL;
    if (!c->mode_valid) {
        g_drm.mode_valid = false;
        return 0;
    }
    struct drm_framebuffer *fb = drm_find_fb(c->fb_id);
    if (!fb) return -EINVAL;
    g_drm.current_fb_id = c->fb_id;
    g_drm.mode_valid = true;
    struct drm_dumb_buffer *d = drm_find_dumb(fb->dumb_handle);
    if (!d) return -EINVAL;
    virtio_gpu_set_scanout(0, d->virtio_res_id, 0, 0, g_drm.fb_width, g_drm.fb_height);
    return 0;
}

/* DRM_IOCTL_MODE_GETCONNECTOR */
static long drm_ioctl_getconnector(void *arg) {
    struct drm_mode_get_connector *c = (struct drm_mode_get_connector *)arg;
    if (c->connector_id != DRM_CONNECTOR_ID) return -EINVAL;
    c->connector_type = DRM_MODE_CONNECTOR_VIRTUAL;
    c->connector_type_id = 1;
    c->connection = DRM_MODE_CONNECTED;
    c->mm_width = 0; c->mm_height = 0;
    c->subpixel = 0;
    c->encoder_id = DRM_ENCODER_ID;
    c->count_encoders = 1;
    c->count_modes = 1;
    c->count_props = 0;
    return 0;
}

/* DRM_IOCTL_MODE_GETENCODER */
static long drm_ioctl_getencoder(void *arg) {
    struct drm_mode_get_encoder *e = (struct drm_mode_get_encoder *)arg;
    if (e->encoder_id != DRM_ENCODER_ID) return -EINVAL;
    e->encoder_type = DRM_MODE_ENCODER_VIRTUAL;
    e->crtc_id = DRM_CRTC_ID;
    e->possible_crtcs = 1;
    e->possible_clones = 0;
    return 0;
}

/* DRM_IOCTL_MODE_GETPLANERES */
static long drm_ioctl_getplaneres(void *arg) {
    struct drm_mode_get_plane_res *p = (struct drm_mode_get_plane_res *)arg;
    p->count_planes = 1;
    return 0;
}

/* DRM_IOCTL_MODE_GETPLANE */
static long drm_ioctl_getplane(void *arg) {
    struct drm_mode_get_plane *p = (struct drm_mode_get_plane *)arg;
    if (p->plane_id != DRM_PLANE_ID) return -EINVAL;
    p->crtc_id = DRM_CRTC_ID;
    p->fb_id = g_drm.current_fb_id;
    p->possible_crtcs = 1;
    p->gamma_size = 0;
    p->count_format_type = 1;
    return 0;
}

/* DRM_IOCTL_MODE_CREATE_DUMB */
static long drm_ioctl_create_dumb(void *arg) {
    struct drm_mode_create_dumb *d = (struct drm_mode_create_dumb *)arg;
    if (d->width != g_drm.fb_width || d->height != g_drm.fb_height || d->bpp != g_drm.fb_bpp)
        return -EINVAL;
    d->pitch = g_drm.fb_pitch;
    d->size = (uint64_t)g_drm.fb_pitch * g_drm.fb_height;

    spin_lock(&g_drm.dumb_lock);
    int handle = drm_alloc_dumb_handle();
    if (handle < 0) { spin_unlock(&g_drm.dumb_lock); return -ENOMEM; }
    struct drm_dumb_buffer *buf = &g_drm.dumbs[handle - 1];
    spin_unlock(&g_drm.dumb_lock);

    buf->width = d->width; buf->height = d->height;
    buf->pitch = d->pitch; buf->size = d->size;

    uint32_t npages = (d->size + PAGE_SIZE - 1) / PAGE_SIZE;
    buf->kernel_vaddr = bfc_alloc_page_data(npages);
    if (!buf->kernel_vaddr) return -ENOMEM;
    __memset(buf->kernel_vaddr, 0, d->size);
    buf->guest_phys = (uint64_t)PHY_ADDR((uintptr_t)buf->kernel_vaddr);

    buf->virtio_res_id = (uint32_t)handle;
    if (virtio_gpu_create_2d(buf->virtio_res_id, d->width, d->height,
                             VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM) < 0) {
        return -EIO;
    }
    if (virtio_gpu_attach_backing(buf->virtio_res_id, buf->guest_phys, d->size) < 0) {
        return -EIO;
    }

    d->handle = (uint32_t)handle;
    return 0;
}

/* DRM_IOCTL_MODE_MAP_DUMB */
static long drm_ioctl_map_dumb(void *arg) {
    struct drm_mode_map_dumb *m = (struct drm_mode_map_dumb *)arg;
    spin_lock(&g_drm.dumb_lock);
    struct drm_dumb_buffer *d = drm_find_dumb((int)m->handle);
    if (!d) { spin_unlock(&g_drm.dumb_lock); return -EINVAL; }
    m->offset = (uint64_t)m->handle << 12;
    spin_unlock(&g_drm.dumb_lock);
    return 0;
}

/* DRM_IOCTL_MODE_DESTROY_DUMB */
static long drm_ioctl_destroy_dumb(void *arg) {
    struct drm_mode_destroy_dumb *d = (struct drm_mode_destroy_dumb *)arg;
    spin_lock(&g_drm.dumb_lock);
    struct drm_dumb_buffer *buf = drm_find_dumb((int)d->handle);
    if (!buf) { spin_unlock(&g_drm.dumb_lock); return -EINVAL; }
    buf->refcount--;
    if (buf->refcount <= 0) {
        __memset(buf, 0, sizeof(*buf));
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
    if (!dref) return -EINVAL;

    spin_lock(&g_drm.fb_lock);
    int fb_id = drm_alloc_fb_id();
    if (fb_id < 0) { spin_unlock(&g_drm.fb_lock); return -ENOMEM; }
    struct drm_framebuffer *fb = &g_drm.fbs[fb_id - 1];
    spin_unlock(&g_drm.fb_lock);

    fb->dumb_handle = (int)f->handle;
    fb->width = f->width; fb->height = f->height;
    fb->pitch = f->pitch; fb->bpp = f->bpp;

    spin_lock(&g_drm.dumb_lock);
    d = drm_find_dumb((int)f->handle);
    if (d) d->refcount++;
    spin_unlock(&g_drm.dumb_lock);

    f->fb_id = (uint32_t)fb_id;
    return 0;
}

/* DRM_IOCTL_MODE_RMFB */
static long drm_ioctl_rmfb(void *arg) {
    uint32_t fb_id = *(uint32_t *)arg;
    spin_lock(&g_drm.fb_lock);
    struct drm_framebuffer *fb = drm_find_fb((int)fb_id);
    if (!fb) { spin_unlock(&g_drm.fb_lock); return -EINVAL; }
    fb->refcount--;
    int dumb_handle = fb->dumb_handle;
    if (fb->refcount <= 0) {
        __memset(fb, 0, sizeof(*fb));
    }
    spin_unlock(&g_drm.fb_lock);

    spin_lock(&g_drm.dumb_lock);
    struct drm_dumb_buffer *d = drm_find_dumb(dumb_handle);
    if (d) d->refcount--;
    spin_unlock(&g_drm.dumb_lock);
    return 0;
}

/* DRM_IOCTL_MODE_PAGE_FLIP */
static long drm_ioctl_page_flip(void *arg) {
    struct drm_mode_crtc_page_flip *p = (struct drm_mode_crtc_page_flip *)arg;
    if (p->crtc_id != DRM_CRTC_ID) return -EINVAL;
    struct drm_framebuffer *fb = drm_find_fb((int)p->fb_id);
    if (!fb) return -EINVAL;
    struct drm_dumb_buffer *d = drm_find_dumb(fb->dumb_handle);
    if (!d) return -EINVAL;

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
    if (!fb) return -EINVAL;
    struct drm_dumb_buffer *d = drm_find_dumb(fb->dumb_handle);
    if (!d) return -EINVAL;
    virtio_gpu_transfer_2d(d->virtio_res_id, 0, 0, d->width, d->height, 0);
    virtio_gpu_flush(d->virtio_res_id, 0, 0, d->width, d->height);
    return 0;
}

/* Main DRM ioctl dispatcher */
long drm_ioctl(uint32_t cmd, void *arg) {
    if (!g_drm.initialized) return -ENODEV;
    switch (cmd) {
    case DRM_IOCTL_VERSION:           return drm_ioctl_version(arg);
    case DRM_IOCTL_GET_CAP:           return drm_ioctl_get_cap(arg);
    case DRM_IOCTL_SET_CLIENT_CAP:    return drm_ioctl_set_client_cap(arg);
    case DRM_IOCTL_SET_MASTER:        return drm_ioctl_set_master();
    case DRM_IOCTL_DROP_MASTER:       return drm_ioctl_drop_master();
    case DRM_IOCTL_MODE_GETRESOURCES: return drm_ioctl_getresources(arg);
    case DRM_IOCTL_MODE_GETCRTC:      return drm_ioctl_getcrtc(arg);
    case DRM_IOCTL_MODE_SETCRTC:      return drm_ioctl_setcrtc(arg);
    case DRM_IOCTL_MODE_GETCONNECTOR: return drm_ioctl_getconnector(arg);
    case DRM_IOCTL_MODE_GETENCODER:   return drm_ioctl_getencoder(arg);
    case DRM_IOCTL_MODE_GETPLANERES:  return drm_ioctl_getplaneres(arg);
    case DRM_IOCTL_MODE_GETPLANE:     return drm_ioctl_getplane(arg);
    case DRM_IOCTL_MODE_CREATE_DUMB:  return drm_ioctl_create_dumb(arg);
    case DRM_IOCTL_MODE_MAP_DUMB:     return drm_ioctl_map_dumb(arg);
    case DRM_IOCTL_MODE_DESTROY_DUMB: return drm_ioctl_destroy_dumb(arg);
    case DRM_IOCTL_MODE_ADDFB:        return drm_ioctl_addfb(arg);
    case DRM_IOCTL_MODE_RMFB:         return drm_ioctl_rmfb(arg);
    case DRM_IOCTL_MODE_PAGE_FLIP:    return drm_ioctl_page_flip(arg);
    case DRM_IOCTL_MODE_DIRTYFB:      return drm_ioctl_dirtyfb(arg);
    case DRM_IOCTL_GET_MAGIC:
    case DRM_IOCTL_AUTH_MAGIC:
    case DRM_IOCTL_GETPROPERTY:
    case DRM_IOCTL_SETPROPERTY:
    case DRM_IOCTL_MODE_GETPROPROB:
    case DRM_IOCTL_MODE_OBJ_GETPROPERTIES:
    case DRM_IOCTL_MODE_OBJ_SETPROPERTY:
    case DRM_IOCTL_PRIME_HANDLE_TO_FD:
    case DRM_IOCTL_PRIME_FD_TO_HANDLE:
        return -ENOSYS;
    default:
        printk(LOG_WARN, "drm_ioctl: unknown cmd 0x%x\n", cmd);
        return -ENOSYS;
    }
}

/* ===== DRM device ops ===== */
int drm_open(xtask *proc, int fd) {
    (void)proc; (void)fd;
    return 0;
}

int drm_close(xtask *proc, int fd) {
    (void)proc; (void)fd;
    return 0;
}

/* forward declarations for ops callbacks defined further below */
static ssize_t drm_read(xtask *proc, int fd, void *buf, size_t count);

static struct dev_ops drm_dev_ops = {
    .driver_pid  = 0,
    .is_block    = false,
    .open        = drm_open,
    .close       = drm_close,
    .ioctl       = drm_ioctl,
    .mmap        = drm_mmap_handler,
    .read        = drm_read,
    .poll        = drm_poll,
};

void drm_dev_register(void) {
    int rc = devtmpfs_create("dri/card0", &drm_dev_ops, NULL);
    if (rc < 0) {
        printk(LOG_ERROR, "drm: failed to create /dev/dri/card0: %d\n", rc);
        return;
    }
    printk(LOG_INFO, "drm: registered /dev/dri/card0\n");
}

/* ===== DRM mmap handler =====
   mmap(fd, offset) where offset = handle << 12 (from MAP_DUMB).
   For Phase 3 (single active dumb buffer at a time) we locate the buffer
   by matching size. Map its physical pages into user space. */
__attribute__((no_sanitize("kernel-address")))
uint64_t drm_mmap_handler(xtask *proc, uint64_t size) {
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
    uint64_t *pml4 = (__force uint64_t *)phys_to_virt((__force phys_addr_t)proc->mm->cr3);
    uint64_t vaddr = proc->mm->mmap_brk;
    uint64_t pte_flags = PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX;

    for (size_t i = 0; i < npages; i++) {
        uint64_t page_phys = target->guest_phys + i * PAGE_SIZE;
        if (!map_user_page_direct(pml4, vaddr + i * PAGE_SIZE, page_phys, pte_flags)) {
            for (size_t j = 0; j < i; j++)
                unmap_user_pages(pml4, vaddr + j * PAGE_SIZE, vaddr + (j + 1) * PAGE_SIZE, 1);
            return 0;
        }
    }

    mmap_region *region = (mmap_region *)kmalloc(sizeof(mmap_region));
    if (!region) {
        for (size_t i = 0; i < npages; i++)
            unmap_user_pages(pml4, vaddr + i * PAGE_SIZE, vaddr + (i + 1) * PAGE_SIZE, 1);
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
    (void)proc; (void)events;
    spin_lock(&g_drm.event_lock);
    __poll revents = g_drm.event_pending ? POLLIN : 0;
    spin_unlock(&g_drm.event_lock);
    return revents;
}

/* ===== DRM read handler (deliver page flip event) ===== */
static ssize_t drm_read(xtask *proc, int fd, void *buf, size_t count) {
    (void)proc; (void)fd;
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
    ev.type = DRM_EVENT_VBLANK;
    ev.length = sizeof(ev);
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

    /* Find PCI device */
    pci_device *pdev = pci_find_device_by_id(VIRTIO_PCI_VENDOR_ID, VIRTIO_PCI_DEVICE_ID);
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
    if (virtio_pci_negotiate_features(&vgpu->vpci, 1ULL << VIRTIO_F_VERSION_1) < 0) {
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
    virtio_pci_write_status(&vgpu->vpci, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER
                                             | VIRTIO_STATUS_FEATURES_OK
                                             | VIRTIO_STATUS_DRIVER_OK);

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

    /* Register /dev/dri/card0 */
    drm_dev_register();

    printk(LOG_INFO, "virtio_gpu: init done\n");
}

dev_driver virtio_gpu_driver = {
    .name              = "virtio_gpu",
    .pci_class         = 0,
    .pci_vendor        = VIRTIO_PCI_VENDOR_ID,
    .pci_device        = VIRTIO_PCI_DEVICE_ID,
    .pci_subsystem_id  = 0,  /* subsystem_id cannot distinguish virtio devices */
    .init              = virtio_gpu_init,
    .ops               = NULL,   /* ops set in Phase 3 (DRM/KMS) */
};

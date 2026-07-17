/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_DRIVER_VIRTIO_GPU_H
#define KERNEL_DRIVER_VIRTIO_GPU_H

#include <stddef.h>
#include <stdint.h>

#include "kernel/driver/virtio_pci.h"
#include "kernel/driver/virtio_ring.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/wait_queue.h"

/* ===== virtio-gpu PCI config (device-specific config space) ===== */
struct virtio_gpu_config {
  uint32_t events_read;
  uint32_t events_clear;
  uint32_t num_scanouts;
  uint32_t num_capsets;
};

/* ===== virtio-gpu command types (ctrlq) ===== */
#define VIRTIO_GPU_F_VIRGL 0
#define VIRTIO_GPU_F_EDID 1
#define VIRTIO_GPU_F_RESOURCE_UUID 2
#define VIRTIO_GPU_F_RESOURCE_BLOB 3
#define VIRTIO_GPU_F_CONTEXT_INIT 4
#define VIRTIO_GPU_F_BLOB_ALIGNMENT 5

#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO 0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D 0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF 0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT 0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH 0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D 0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING 0x0107
#define VIRTIO_GPU_CMD_GET_CAPSET_INFO 0x0108
#define VIRTIO_GPU_CMD_GET_CAPSET 0x0109
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB 0x010C
#define VIRTIO_GPU_CMD_SET_SCANOUT_BLOB 0x010D
#define VIRTIO_GPU_CMD_CTX_CREATE 0x0200
#define VIRTIO_GPU_CMD_CTX_DESTROY 0x0201
#define VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE 0x0202
#define VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE 0x0203
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_3D 0x0204
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D 0x0205
#define VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D 0x0206
#define VIRTIO_GPU_CMD_SUBMIT_3D 0x0207
#define VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB 0x0208
#define VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB 0x0209
#define VIRTIO_GPU_CMD_UPDATE_CURSOR 0x0300
#define VIRTIO_GPU_CMD_MOVE_CURSOR 0x0301

/* ===== virtio-gpu response types ===== */
#define VIRTIO_GPU_RESP_OK_NODATA 0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO 0x1101
#define VIRTIO_GPU_RESP_OK_CAPSET_INFO 0x1102
#define VIRTIO_GPU_RESP_OK_CAPSET 0x1103
#define VIRTIO_GPU_RESP_ERR_UNSPEC 0x1200
#define VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY 0x1201
#define VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID 0x1202
#define VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID 0x1203
#define VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID 0x1204
#define VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER 0x1205
#define VIRTIO_GPU_RESP_OK_MAP_INFO 0x1104

/* ===== virtio-gpu formats ===== */
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM 1
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM 2
#define VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM 3
#define VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM 4
#define VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM 67
#define VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM 68
#define VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM 121
#define VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM 134

/* ===== virtio-gpu command/response headers ===== */
struct virtio_gpu_ctrl_hdr {
  uint32_t type;
  uint32_t flags;
  uint64_t fence_id;
  uint32_t ctx_id;
  uint8_t ring_idx; /* per-ring fence (VIRTIO_GPU_FLAG_INFO_RING_IDX) */
  uint8_t padding[3];
};

#define VIRTIO_GPU_FLAG_FENCE (1 << 0)
#define VIRTIO_GPU_FLAG_INFO_RING_IDX (1 << 1)

#define VIRTIO_GPU_CONTEXT_INIT_CAPSET_ID_MASK 0x000000ff

struct virtio_gpu_ctx_create {
  struct virtio_gpu_ctrl_hdr hdr;
  uint32_t nlen;
  uint32_t context_init; /* capset_id in low byte */
  char debug_name[64];
};

struct virtio_gpu_resource_create_blob {
  struct virtio_gpu_ctrl_hdr hdr;
  uint32_t resource_id;
  uint32_t blob_mem;
  uint32_t blob_flags;
  uint32_t nr_entries; /* 0 for HOST3D (no guest backing) */
  uint64_t blob_id;
  uint64_t size;
};

struct virtio_gpu_resource_map_blob {
  struct virtio_gpu_ctrl_hdr hdr;
  uint32_t resource_id;
  uint32_t padding;
};

/* MAP_BLOB response: map_info + host-visible offset. */
struct virtio_gpu_resp_map_info {
  struct virtio_gpu_ctrl_hdr hdr;
  uint32_t map_info; /* VIRTIO_GPU_MAP_INFO_* */
  uint32_t padding;
  uint64_t offset; /* host-visible offset */
};

/* RESOURCE_CREATE_2D command */
struct virtio_gpu_resource_create_2d {
  struct virtio_gpu_ctrl_hdr hdr;
  uint32_t resource_id;
  uint32_t format;
  uint32_t width;
  uint32_t height;
};

/* RESOURCE_UNREF command */
struct virtio_gpu_resource_unref {
  struct virtio_gpu_ctrl_hdr hdr;
  uint32_t resource_id;
  uint32_t padding;
};

/* ATTACH_BACKING command */
struct virtio_gpu_mem_entry {
  uint64_t addr;   /* guest physical address */
  uint32_t length; /* length in bytes */
  uint32_t padding;
};

struct virtio_gpu_resource_attach_backing {
  struct virtio_gpu_ctrl_hdr hdr;
  uint32_t resource_id;
  uint32_t nr_entries;
  /* followed by nr_entries * sizeof(virtio_gpu_mem_entry) bytes */
};

/* SET_SCANOUT command */
struct virtio_gpu_set_scanout {
  struct virtio_gpu_ctrl_hdr hdr;
  struct virtio_gpu_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
  } r;
  uint32_t scanout_id;
  uint32_t resource_id;
};

/* TRANSFER_TO_HOST_2D command */
struct virtio_gpu_transfer_to_host_2d {
  struct virtio_gpu_ctrl_hdr hdr;
  struct virtio_gpu_rect r;
  uint64_t offset;
  uint32_t resource_id;
  uint32_t padding;
};

/* RESOURCE_FLUSH command */
struct virtio_gpu_resource_flush {
  struct virtio_gpu_ctrl_hdr hdr;
  struct virtio_gpu_rect r;
  uint32_t resource_id;
  uint32_t padding;
};

/* GET_DISPLAY_INFO response */
struct virtio_gpu_resp_display_info {
  struct virtio_gpu_ctrl_hdr hdr;
  struct virtio_gpu_display_one {
    struct virtio_gpu_rect r;
    uint32_t enabled;
    uint32_t flags;
  } pmodes[16];
};

/* Generic response (for commands that only return a header) */
struct virtio_gpu_ctrl_hdr_response {
  struct virtio_gpu_ctrl_hdr hdr;
};

/* GET_CAPSET_INFO / GET_CAPSET responses */
struct virtio_gpu_resp_capset_info {
  struct virtio_gpu_ctrl_hdr hdr;
  uint32_t capset_id;
  uint32_t capset_max_version;
  uint32_t capset_max_size;
  uint32_t padding;
};

struct virtio_gpu_get_capset {
  struct virtio_gpu_ctrl_hdr hdr;
  uint32_t capset_id;
  uint32_t capset_version;
  uint32_t padding;
};

struct virtio_gpu_resp_capset {
  struct virtio_gpu_ctrl_hdr hdr;
  uint8_t capset_data[];
};

/* ===== virtio-gpu device state ===== */
#define VIRTIO_GPU_CTRLQ_INDEX 0

struct virtio_gpu_device {
  struct virtio_pci_dev vpci;
  struct virtqueue ctrlq;
  struct virtio_gpu_config config;

  /* command synchronization: cmd_lock serializes vring submission;
     cmd_wq + per-command ctx track completion for multiple waiters */
  spinlock cmd_lock;
  wait_queue_head cmd_wq; /* wait queue for processes sleeping in send_cmd */
};

/* ===== API ===== */
/* Initialize virtio-gpu device: transport + ctrlq + MSI-X + ISR. */
void virtio_gpu_init(void);

/* High-level command wrappers (all synchronous): */
int virtio_gpu_create_2d(uint32_t resource_id, uint32_t width, uint32_t height,
                         uint32_t format);
int virtio_gpu_attach_backing(uint32_t resource_id, uint64_t guest_phys,
                              uint32_t length);
int virtio_gpu_set_scanout(uint32_t scanout_id, uint32_t resource_id,
                           uint32_t x, uint32_t y, uint32_t w, uint32_t h);
int virtio_gpu_transfer_2d(uint32_t resource_id, uint32_t x, uint32_t y,
                           uint32_t w, uint32_t h, uint64_t offset);
int virtio_gpu_flush(uint32_t resource_id, uint32_t x, uint32_t y, uint32_t w,
                     uint32_t h);
int virtio_gpu_resource_unref(uint32_t resource_id);

/* 3D command senders (plan1/plan2). send_cmd_3d wraps send_cmd with a
 * ctrl_hdr-bearing command + generic hdr response. */
int virtio_gpu_send_cmd_3d(struct virtio_gpu_device *vgpu, void *cmd_buf,
                           size_t cmd_len, void *resp_buf, size_t resp_len);

extern struct virtio_gpu_device g_virtio_gpu;

#endif /* KERNEL_DRIVER_VIRTIO_GPU_H */

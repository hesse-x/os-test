/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_DRIVER_DRM_INTERNAL_H
#define KERNEL_DRIVER_DRM_INTERNAL_H

#include "kernel/bsd/devtmpfs.h" /* __poll, xtask via inode.h chain */
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/wait_queue.h"
#include <stdbool.h>
#include <stdint.h>

/* ===== Static KMS object IDs (1:1 hardcoded topology) ===== */
#define DRM_CRTC_ID 1
#define DRM_CONNECTOR_ID 2
#define DRM_ENCODER_ID 3
#define DRM_PLANE_ID 4
#define DRM_PLANE_TYPE_OVERLAY 0
#define DRM_PLANE_TYPE_PRIMARY 1
#define DRM_PLANE_TYPE_CURSOR 2

/* ===== Property infrastructure (Phase C) ===== */
#define DRM_MAX_PROPERTIES 32
#define DRM_MAX_PROPS_PER_OBJECT 16
#define DRM_MAX_BLOBS 16
#define DRM_PROP_NAME_LEN 32

enum drm_prop_type {
  DRM_PROP_RANGE,
  DRM_PROP_ENUM,
  DRM_PROP_BLOB,
  DRM_PROP_OBJECT,
};

struct drm_prop_enum {
  uint64_t value;
  char name[DRM_PROP_NAME_LEN];
};

struct drm_property {
  uint32_t prop_id; /* 1-based, 0 = free slot */
  bool allocated;
  char name[DRM_PROP_NAME_LEN];
  enum drm_prop_type type;

  /* RANGE: {min, max}, ENUM: {count, values[], names[]} */
  uint32_t range_min;
  uint32_t range_max;

  int enum_count;
  struct drm_prop_enum enums[16];

  bool is_immutable; /* DRM_MODE_PROP_IMMUTABLE */

  /* BLOB: only flag (actual data stored in drm_blob) */
};

struct drm_blob {
  uint32_t blob_id; /* 1-based, 0 = free slot */
  bool allocated;
  int refcount;
  size_t length;
  void *data; /* kmalloc'd copy */
};

struct drm_object_props {
  uint32_t prop_ids[DRM_MAX_PROPS_PER_OBJECT];
  uint64_t prop_values[DRM_MAX_PROPS_PER_OBJECT];
  int count;
  spinlock lock;
};

/* ===== Software cursor (Phase C) ===== */
#define CURSOR_WIDTH 64
#define CURSOR_HEIGHT 64
#define CURSOR_SIZE (CURSOR_WIDTH * CURSOR_HEIGHT * 4) /* 32bpp ARGB */

struct drm_cursor {
  bool enabled;
  int16_t x; /* current cursor position (screen coords) */
  int16_t y;
  int16_t hotspot_x; /* from CURSOR2 */
  int16_t hotspot_y;
  uint32_t buffer[CURSOR_WIDTH * CURSOR_HEIGHT]; /* ARGB cursor bitmap */
  bool dirty; /* cursor position/content changed since last flip */
  spinlock lock;
};

extern struct drm_cursor g_drm_cursor;

/* ===== Resource pool sizes (shared across per-fd tracking, dumb, fb) ===== */
#define MAX_DUMB_BUFFERS 16
#define MAX_FRAMEBUFFERS 16

/* ===== per-fd tracking (Phase C) ===== */
#define MAX_DRM_FDS 8

#define MAX_CAPSETS 8
#define MAX_CTX_IDS 256

/* virgl legacy (v1) resources: GEM handles allocated from VIRGL_HANDLE_BASE
 * upward so they never collide numerically with dumb (1..16) handles, letting
 * GEM_CLOSE / RESOURCE_INFO dispatch by a single range test. */
#define MAX_VIRGL_RESOURCES 128
#define VIRGL_HANDLE_BASE 0x1000u

#define MAX_FENCES 256

/* Fence: one per submitted EXECBUFFER with FENCE_FD_OUT (sync_file). */
struct drm_fence {
  uint32_t ctx_id; /* 0 = free slot */
  uint8_t ring_idx;
  uint64_t fence_id;
  bool signaled;
  refcount_t refcount; /* see 2A-3 / 2D-1: sync_file fd holds a ref */
  spinlock lock;       /* irqsave: signal runs in ISR, add runs in process */
  wait_queue_head wq;  /* tasks waiting for signal */
};

/* virgl legacy (v1) resource: kernel-allocated guest backing attached to a
 * host 3D resource. The winsys passes only bo_handle to TRANSFER/WAIT, so the
 * kernel persists bo_handle→res_handle here and resolves it internally. */
struct drm_virgl_resource {
  uint32_t bo_handle;  /* == VIRGL_HANDLE_BASE + index, 0 = free slot */
  uint32_t res_handle; /* host virtio-gpu resource id (== bo_handle) */
  uint64_t guest_phys; /* guest physical address of backing pages */
  void *kernel_vaddr;  /* kernel virtual address of backing pages */
  uint64_t size;
  int refcount;
  /* A resource may be shared through PRIME and used by several contexts. */
  uint32_t ctx_attach_bitmap[(MAX_CTX_IDS + 31) / 32];
};

struct drm_prime_object {
  uint32_t handle;
  bool is_virgl;
};

/* Cached capset info fetched at init via GET_CAPSET_INFO/GET_CAPSET. */
struct drm_capset {
  uint32_t id;
  uint32_t ver;
  uint32_t size;
  void *data; /* kmalloc'd capset payload */
};

struct drm_file {
  int fd;                       /* system fd number, 0 = free slot */
  xtask *proc;                  /* owning process */
  bool is_master;               /* this fd holds master */
  uint32_t authenticated_magic; /* the magic this fd got via GET_MAGIC */
  bool auth_valid;              /* magic has been authenticated */
  bool used;                    /* slot in use */
  bool is_render;               /* render node (renderD128): no master/auth */

  /* Venus 3D context (plan1 CONTEXT_INIT) */
  uint32_t ctx_id;    /* 0 = no context */
  uint32_t num_rings; /* rings allocated by CONTEXT_INIT */
  uint32_t poll_rings_mask;
  uint64_t *ring_fence_counters; /* per-ring fence counter (plan2 uses) */

  /* virgl legacy (v1) resources created by this fd (for drm_close cleanup) */
  int created_virgl_handles[MAX_VIRGL_RESOURCES];
  int created_virgl_count;

  /* Tracking of resources owned by this fd */
  int created_fb_ids[MAX_FRAMEBUFFERS];
  int created_fb_count;
  int created_dumb_handles[MAX_DUMB_BUFFERS];
  int created_dumb_count;
};

extern struct drm_file g_drm_files[MAX_DRM_FDS];
extern spinlock g_drm_files_lock;

/* ===== Default mode: 800x600@60 (runtime-overridable via g_drm.fb_*) ===== */
#define DRM_FB_WIDTH 800
#define DRM_FB_HEIGHT 600
#define DRM_FB_BPP 32
#define DRM_FB_PITCH (DRM_FB_WIDTH * 4)
#define DRM_FB_SIZE (DRM_FB_PITCH * DRM_FB_HEIGHT)

/* ===== dumb buffer (kernel-allocated, refcounted) ===== */

struct drm_dumb_buffer {
  int handle; /* 1-based handle, 0 = free slot */
  int refcount;
  uint64_t guest_phys; /* guest physical address of buffer pages */
  void *kernel_vaddr;  /* kernel virtual address */
  uint32_t width;
  uint32_t height;
  uint32_t pitch;
  uint64_t size;
  uint32_t
      virtio_res_id; /* virtio-gpu host resource id (assigned on create_2d) */
};

/* ===== framebuffer (references a dumb buffer, refcounted) ===== */

struct drm_framebuffer {
  int fb_id; /* 1-based, 0 = free slot */
  int refcount;
  int dumb_handle; /* references drm_dumb_buffer.handle */
  bool is_virgl;
  uint32_t width;
  uint32_t height;
  uint32_t pitch;
  uint32_t bpp;
};

/* ===== DRM device ===== */
struct drm_device {
  bool initialized;
  bool is_master;    /* SET_MASTER/DROP_MASTER */
  int magic_counter; /* GET_MAGIC: auto-increment magic number */

  /* current CRTC state */
  uint32_t current_fb_id;
  bool mode_valid;

  /* runtime display mode (defaults from DRM_FB_* macros, overridable later) */
  uint32_t fb_width;
  uint32_t fb_height;
  uint32_t fb_bpp;
  uint32_t fb_pitch;

  /* capset cache (plan1 GET_CAPS) */
  struct drm_capset capsets[MAX_CAPSETS];
  uint32_t num_capsets;
  spinlock capset_lock;

  /* ctx_id allocation pool (plan1 CONTEXT_INIT) */
  uint32_t ctx_id_bitmap[(MAX_CTX_IDS + 31) /
                         32]; /* bit i set = ctx_id i+1 in use */
  spinlock ctx_id_lock;

  /* virgl legacy (v1) resource table. Handles start at VIRGL_HANDLE_BASE. */
  struct drm_virgl_resource virgl_res[MAX_VIRGL_RESOURCES];
  uint32_t next_virgl_handle; /* monotonic from VIRGL_HANDLE_BASE */
  spinlock virgl_lock;

  /* fence table (plan2). slot free iff ctx_id==0. */
  struct drm_fence fences[MAX_FENCES];
  spinlock fence_lock; /* protects slot alloc/find across the table */

  /* dumb buffer table */
  struct drm_dumb_buffer dumbs[MAX_DUMB_BUFFERS];
  int next_dumb_handle;
  spinlock dumb_lock;

  /* framebuffer table */
  struct drm_framebuffer fbs[MAX_FRAMEBUFFERS];
  int next_fb_id;
  spinlock fb_lock;

  /* page flip event queue (single-entry, immediate delivery) */
  spinlock event_lock;
  bool event_pending;
  uint32_t event_sequence;
  uint64_t event_user_data;

  /* Unified vblank/event wait queue: drm_poll waiters register here via
   * file_wq_get, and drm_ioctl_page_flip wakes it after setting event_pending.
   * Without this, waiters sat on a per-file f->wq that page_flip never woke
   * (poll(deadline=0) + no EVENT flag = permanent deadlock). See bug.md. */
  wait_queue_head event_wq;
};

extern struct drm_device g_drm;
extern struct dev_ops
    drm_dev_ops; /* card0 ops — file_wq_get identifies DRM
                  * card0 fds (inode->i_priv == &drm_dev_ops)
                  * to route poll waiters to g_drm.event_wq. */
extern struct drm_property g_drm_properties[DRM_MAX_PROPERTIES];
extern int g_drm_next_prop_id;
extern struct drm_blob g_drm_blobs[DRM_MAX_BLOBS];
extern int g_drm_next_blob_id;

/* ===== DRM mmap handler (called from dev_ops.mmap) ===== */
uint64_t drm_mmap_handler(xtask *proc, uint64_t size, uint64_t offset);

/* ===== DRM poll handler ===== */
__poll drm_poll(xtask *proc, int events);

/* ===== Fence lifecycle (plan2). drm_fence_put reclaims the slot when the
 * last ref drops; called from sync_file fd close (proc.c file_put) and
 * EXECBUFFER error paths, so non-static. ===== */
void drm_fence_put(struct drm_fence *fence);

/* Drop the PRIME fd's buffer reference and release its wrapper object. */
void drm_prime_object_put(struct drm_prime_object *object);

/* Read-only signaled probe for sync_file poll (file_poll.c calls this to avoid
 * pulling the driver-layer drm_internal.h into the BSD layer). */
bool drm_fence_is_signaled(struct drm_fence *fence);

#endif /* KERNEL_DRIVER_DRM_INTERNAL_H */

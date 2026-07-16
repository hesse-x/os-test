/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_DRIVER_DRM_INTERNAL_H
#define KERNEL_DRIVER_DRM_INTERNAL_H

#include "kernel/bsd/devtmpfs.h" /* __poll, xtask via inode.h chain */
#include "kernel/xcore/spinlock.h"
#include <stdbool.h>
#include <stdint.h>

/* ===== Static KMS object IDs (1:1 hardcoded topology) ===== */
#define DRM_CRTC_ID 1
#define DRM_CONNECTOR_ID 2
#define DRM_ENCODER_ID 3
#define DRM_PLANE_ID 4

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

struct drm_file {
  int fd;                       /* system fd number, 0 = free slot */
  xtask *proc;                  /* owning process */
  bool is_master;               /* this fd holds master */
  uint32_t authenticated_magic; /* the magic this fd got via GET_MAGIC */
  bool auth_valid;              /* magic has been authenticated */
  bool used;                    /* slot in use */
  bool is_render;               /* render node (renderD128): no master/auth */

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
};

extern struct drm_device g_drm;
extern struct drm_property g_drm_properties[DRM_MAX_PROPERTIES];
extern int g_drm_next_prop_id;
extern struct drm_blob g_drm_blobs[DRM_MAX_BLOBS];
extern int g_drm_next_blob_id;

/* ===== DRM ioctl handler (called from sys_ioctl via dev_ops.ioctl) ===== */
long drm_ioctl(uint32_t cmd, void *arg);

/* ===== DRM mmap handler (called from dev_ops.mmap) ===== */
uint64_t drm_mmap_handler(xtask *proc, uint64_t size, uint64_t offset);

/* ===== DRM poll handler ===== */
__poll drm_poll(xtask *proc, int events);

/* ===== DRM open/close ===== */
int drm_open(xtask *proc, int fd);
int drm_close(xtask *proc, int fd);

#endif /* KERNEL_DRIVER_DRM_INTERNAL_H */

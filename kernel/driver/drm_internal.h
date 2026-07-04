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

/* ===== Default mode: 800x600@60 (runtime-overridable via g_drm.fb_*) ===== */
#define DRM_FB_WIDTH 800
#define DRM_FB_HEIGHT 600
#define DRM_FB_BPP 32
#define DRM_FB_PITCH (DRM_FB_WIDTH * 4)
#define DRM_FB_SIZE (DRM_FB_PITCH * DRM_FB_HEIGHT)

/* ===== dumb buffer (kernel-allocated, refcounted) ===== */
#define MAX_DUMB_BUFFERS 16

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
#define MAX_FRAMEBUFFERS 16

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

/* ===== DRM ioctl handler (called from sys_ioctl via dev_ops.ioctl) ===== */
long drm_ioctl(uint32_t cmd, void *arg);

/* ===== DRM mmap handler (called from dev_ops.mmap) ===== */
uint64_t drm_mmap_handler(xtask *proc, uint64_t size);

/* ===== DRM poll handler ===== */
__poll drm_poll(xtask *proc, int events);

/* ===== DRM open/close ===== */
int drm_open(xtask *proc, int fd);
int drm_close(xtask *proc, int fd);

#endif /* KERNEL_DRIVER_DRM_INTERNAL_H */

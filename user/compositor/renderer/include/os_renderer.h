/*
 * Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#ifndef OS_COMPOSITOR_RENDERER_H
#define OS_COMPOSITOR_RENDERER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OS_RENDERER_API_VERSION 1u

struct os_renderer;
struct os_output_buffer;
struct os_texture;
struct os_frame;
struct os_mesh;

enum os_renderer_result {
  OS_RENDERER_OK,
  OS_RENDERER_INVALID_ARGUMENT,
  OS_RENDERER_UNSUPPORTED,
  OS_RENDERER_OUT_OF_MEMORY,
  OS_RENDERER_FATAL,
};

struct os_renderer_options {
  uint32_t size;
  uint32_t version;
  uint32_t flags;
};

struct os_damage {
  uint32_t size;
  const void *region;
};

struct os_submit_result {
  uint32_t size;
  uint64_t frame_id;
  int release_fence_fd;
};

struct os_mesh_draw {
  uint32_t size;
  const struct os_mesh *mesh;
  const struct os_texture *texture;
  float opacity;
};

struct os_frame *os_renderer_begin_frame(struct os_renderer *renderer,
                                         struct os_output_buffer *output,
                                         const struct os_damage *damage);
bool os_frame_add_mesh(struct os_frame *frame, const struct os_mesh_draw *draw);
enum os_renderer_result os_renderer_submit(struct os_frame *frame,
                                           struct os_submit_result *result);
void os_frame_abort(struct os_frame *frame);
void os_renderer_destroy(struct os_renderer *renderer);

#endif

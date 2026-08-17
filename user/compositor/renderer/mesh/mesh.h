/*
 * Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#ifndef OS_COMPOSITOR_MESH_H
#define OS_COMPOSITOR_MESH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OS_MESH_MAX_VERTICES 65536u
#define OS_MESH_MAX_INDICES 393216u

struct os_vec2 {
  float x, y;
};

struct os_mesh_vertex {
  float position[2];
  float uv[2];
  uint32_t color_rgba8;
  float coverage;
  uint32_t flags;
};

struct os_mesh {
  struct os_mesh_vertex *vertices;
  uint32_t *indices;
  size_t vertex_count;
  size_t index_count;
  size_t vertex_capacity;
  size_t index_capacity;
};

struct os_grid_spec {
  uint32_t size;
  uint32_t columns;
  uint32_t rows;
  uint32_t color_rgba8;
};

typedef bool (*os_mesh_deform_fn)(void *data, float u, float v, float progress,
                                  struct os_vec2 *out);

bool os_mesh_build_grid(const struct os_grid_spec *spec,
                        os_mesh_deform_fn deform, void *data, float progress,
                        struct os_mesh *out);

#endif

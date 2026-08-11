/* Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#include "mesh.h"

#include <math.h>

bool os_mesh_build_grid(const struct os_grid_spec *spec,
                        os_mesh_deform_fn deform, void *data, float progress,
                        struct os_mesh *out) {
  if (out != NULL)
    out->vertex_count = out->index_count = 0;
  if (spec == NULL || out == NULL || deform == NULL ||
      spec->size != sizeof(*spec) || spec->columns == 0 || spec->rows == 0 ||
      !isfinite(progress) || progress < 0.0f || progress > 1.0f)
    return false;
  size_t vertices = (size_t)(spec->columns + 1) * (spec->rows + 1);
  size_t indices = (size_t)spec->columns * spec->rows * 6;
  if (vertices > OS_MESH_MAX_VERTICES || indices > OS_MESH_MAX_INDICES ||
      out->vertices == NULL || out->indices == NULL ||
      out->vertex_capacity < vertices || out->index_capacity < indices)
    return false;

  for (uint32_t row = 0; row <= spec->rows; ++row) {
    for (uint32_t column = 0; column <= spec->columns; ++column) {
      float u = (float)column / spec->columns;
      float v = (float)row / spec->rows;
      struct os_vec2 position;
      if (!deform(data, u, v, progress, &position) || !isfinite(position.x) ||
          !isfinite(position.y)) {
        return false;
      }
      size_t index = (size_t)row * (spec->columns + 1) + column;
      out->vertices[index] =
          (struct os_mesh_vertex){.position = {position.x, position.y},
                                  .uv = {u, v},
                                  .color_rgba8 = spec->color_rgba8,
                                  .coverage = 1.0f};
    }
  }
  float orientation = 0.0f;
  for (uint32_t row = 0; row < spec->rows; ++row) {
    for (uint32_t column = 0; column < spec->columns; ++column) {
      size_t a = (size_t)row * (spec->columns + 1) + column;
      size_t b = a + 1;
      size_t c = a + spec->columns + 1;
      size_t d = c + 1;
      const struct os_mesh_vertex *v[] = {&out->vertices[a], &out->vertices[b],
                                          &out->vertices[c], &out->vertices[d]};
      float cross[] = {(v[2]->position[0] - v[0]->position[0]) *
                               (v[1]->position[1] - v[0]->position[1]) -
                           (v[2]->position[1] - v[0]->position[1]) *
                               (v[1]->position[0] - v[0]->position[0]),
                       (v[2]->position[0] - v[1]->position[0]) *
                               (v[3]->position[1] - v[1]->position[1]) -
                           (v[2]->position[1] - v[1]->position[1]) *
                               (v[3]->position[0] - v[1]->position[0])};
      for (size_t i = 0; i < 2; ++i) {
        if (!isfinite(cross[i]) || cross[i] == 0.0f ||
            (orientation != 0.0f && (cross[i] > 0.0f) != (orientation > 0.0f)))
          return false;
        orientation = cross[i];
      }
    }
  }
  size_t cursor = 0;
  for (uint32_t row = 0; row < spec->rows; ++row) {
    for (uint32_t column = 0; column < spec->columns; ++column) {
      uint32_t a = row * (spec->columns + 1) + column;
      uint32_t b = a + 1;
      uint32_t c = a + spec->columns + 1;
      uint32_t d = c + 1;
      out->indices[cursor++] = a;
      out->indices[cursor++] = c;
      out->indices[cursor++] = b;
      out->indices[cursor++] = b;
      out->indices[cursor++] = c;
      out->indices[cursor++] = d;
    }
  }
  out->vertex_count = vertices;
  out->index_count = indices;
  return true;
}

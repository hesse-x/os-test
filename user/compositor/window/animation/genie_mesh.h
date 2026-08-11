/* Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#ifndef OS_COMPOSITOR_GENIE_MESH_H
#define OS_COMPOSITOR_GENIE_MESH_H

#include <stdbool.h>
#include <stdint.h>

#include "../../renderer/mesh/mesh.h"

enum os_genie_direction {
  OS_GENIE_MINIMIZE = 1,
  OS_GENIE_RESTORE = -1,
};

enum os_genie_status {
  OS_GENIE_IDLE,
  OS_GENIE_RUNNING,
  OS_GENIE_FINISHED,
  OS_GENIE_CANCELLED,
};

struct os_genie_target {
  uint64_t id;
  uint64_t generation;
  float x, y, width, height;
};

struct os_genie_snapshot {
  void *handle;
  uint32_t width, height;
  void (*release)(void *handle);
};

struct os_genie_animation {
  struct os_genie_snapshot snapshot;
  struct os_genie_target target;
  float source_x, source_y, source_width, source_height;
  float progress;
  uint64_t elapsed_ns;
  uint64_t duration_ns;
  enum os_genie_direction direction;
  enum os_genie_status status;
};

bool os_genie_begin(struct os_genie_animation *animation,
                    const struct os_genie_snapshot *snapshot,
                    const struct os_genie_target *target, float source_x,
                    float source_y, float source_width, float source_height,
                    uint64_t duration_ns, enum os_genie_direction direction);
bool os_genie_step(struct os_genie_animation *animation, uint64_t elapsed_ns,
                   const struct os_genie_target *current_target,
                   struct os_mesh *mesh, uint32_t columns, uint32_t rows);
bool os_genie_reverse(struct os_genie_animation *animation);
void os_genie_cancel(struct os_genie_animation *animation);
void os_genie_destroy(struct os_genie_animation *animation);

#endif

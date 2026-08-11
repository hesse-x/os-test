/* Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#include "genie_mesh.h"

#include <math.h>
#include <string.h>

struct deform_data {
  const struct os_genie_animation *animation;
};

static float clamp01(float value) {
  return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

static float smoothstep(float value) {
  value = clamp01(value);
  return value * value * (3.0f - 2.0f * value);
}

static float cubic_bezier(float from, float control1, float control2, float to,
                          float amount) {
  float inverse = 1.0f - amount;
  return inverse * inverse * inverse * from +
         3.0f * inverse * inverse * amount * control1 +
         3.0f * inverse * amount * amount * control2 +
         amount * amount * amount * to;
}

static bool deform(void *opaque, float u, float v, float progress,
                   struct os_vec2 *out) {
  const struct os_genie_animation *animation =
      ((const struct deform_data *)opaque)->animation;
  float delay = (1.0f - v) * 0.58f;
  float travel = smoothstep((progress - delay) / (1.0f - delay));
  float source_center = animation->source_x + animation->source_width * 0.5f;
  float target_center = animation->target.x + animation->target.width * 0.5f;
  float center_delta = target_center - source_center;
  float center =
      cubic_bezier(source_center, source_center + center_delta * 0.10f,
                   target_center - center_delta * 0.16f, target_center, travel);
  float width = cubic_bezier(
      animation->source_width, animation->source_width * 1.01f,
      animation->target.width * 1.85f, animation->target.width, travel);
  float source_y = animation->source_y + animation->source_height * v;
  float target_y = animation->target.y + animation->target.height * v;
  const float pi = 3.14159265358979323846f;
  float fold_envelope = sinf(pi * v) * sinf(pi * progress);
  width *=
      1.0f - 0.045f * fold_envelope * sinf(pi * clamp01(v + progress * 0.55f));
  out->x = center + (u - 0.5f) * width;
  out->y = source_y + (target_y - source_y) * travel;
  return isfinite(out->x) && isfinite(out->y);
}

static void release_snapshot(struct os_genie_animation *animation) {
  if (animation->snapshot.handle != NULL && animation->snapshot.release != NULL)
    animation->snapshot.release(animation->snapshot.handle);
  memset(&animation->snapshot, 0, sizeof(animation->snapshot));
}

bool os_genie_begin(struct os_genie_animation *animation,
                    const struct os_genie_snapshot *snapshot,
                    const struct os_genie_target *target, float source_x,
                    float source_y, float source_width, float source_height,
                    uint64_t duration_ns, enum os_genie_direction direction) {
  if (animation == NULL || snapshot == NULL || snapshot->handle == NULL ||
      snapshot->release == NULL || target == NULL || target->id == 0 ||
      target->generation == 0 || !isfinite(source_x) || !isfinite(source_y) ||
      !isfinite(source_width) || !isfinite(source_height) ||
      source_width <= 0.0f || source_height <= 0.0f || target->width <= 0.0f ||
      target->height <= 0.0f || duration_ns == 0 ||
      (direction != OS_GENIE_MINIMIZE && direction != OS_GENIE_RESTORE))
    return false;
  memset(animation, 0, sizeof(*animation));
  animation->snapshot = *snapshot;
  animation->target = *target;
  animation->source_x = source_x;
  animation->source_y = source_y;
  animation->source_width = source_width;
  animation->source_height = source_height;
  animation->duration_ns = duration_ns;
  animation->direction = direction;
  animation->progress = direction == OS_GENIE_MINIMIZE ? 0.0f : 1.0f;
  animation->elapsed_ns = direction == OS_GENIE_MINIMIZE ? 0 : duration_ns;
  animation->status = OS_GENIE_RUNNING;
  return true;
}

bool os_genie_step(struct os_genie_animation *animation, uint64_t elapsed_ns,
                   const struct os_genie_target *current_target,
                   struct os_mesh *mesh, uint32_t columns, uint32_t rows) {
  if (animation == NULL || animation->status != OS_GENIE_RUNNING ||
      current_target == NULL || mesh == NULL)
    return false;
  if (current_target->id != animation->target.id ||
      current_target->generation != animation->target.generation) {
    os_genie_cancel(animation);
    return false;
  }
  if (animation->direction == OS_GENIE_MINIMIZE) {
    uint64_t remaining = animation->duration_ns - animation->elapsed_ns;
    animation->elapsed_ns += elapsed_ns > remaining ? remaining : elapsed_ns;
  } else {
    animation->elapsed_ns = elapsed_ns > animation->elapsed_ns
                                ? 0
                                : animation->elapsed_ns - elapsed_ns;
  }
  animation->progress =
      (float)((double)animation->elapsed_ns / animation->duration_ns);
  struct deform_data data = {.animation = animation};
  struct os_grid_spec spec = {.size = sizeof(spec),
                              .columns = columns,
                              .rows = rows,
                              .color_rgba8 = UINT32_MAX};
  if (!os_mesh_build_grid(&spec, deform, &data, animation->progress, mesh))
    return false;
  bool complete = animation->direction == OS_GENIE_MINIMIZE
                      ? animation->elapsed_ns == animation->duration_ns
                      : animation->elapsed_ns == 0;
  if (complete) {
    animation->status = OS_GENIE_FINISHED;
    release_snapshot(animation);
  }
  return true;
}

bool os_genie_reverse(struct os_genie_animation *animation) {
  if (animation == NULL || animation->status != OS_GENIE_RUNNING)
    return false;
  animation->direction = animation->direction == OS_GENIE_MINIMIZE
                             ? OS_GENIE_RESTORE
                             : OS_GENIE_MINIMIZE;
  return true;
}

void os_genie_cancel(struct os_genie_animation *animation) {
  if (animation == NULL || animation->status != OS_GENIE_RUNNING)
    return;
  animation->status = OS_GENIE_CANCELLED;
  release_snapshot(animation);
}

void os_genie_destroy(struct os_genie_animation *animation) {
  if (animation == NULL)
    return;
  release_snapshot(animation);
  memset(animation, 0, sizeof(*animation));
}

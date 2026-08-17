/*
 * Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#ifndef OS_COMPOSITOR_EFFECT_MANAGER_H
#define OS_COMPOSITOR_EFFECT_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "effect_state.h"
#include "trusted_client.h"

#define OS_EFFECT_MAX_SURFACES 4u

struct wl_client;

struct os_effect_surface {
  const void *surface;
  struct os_effect_state state;
  uint64_t id;
  bool occupied;
};

struct os_effect_manager {
  struct os_trusted_client trusted;
  struct os_effect_surface surfaces[OS_EFFECT_MAX_SURFACES];
  uint32_t max_radius;
  uint64_t max_region_pixels;
  uint64_t next_id;
};

void os_effect_manager_init(struct os_effect_manager *manager,
                            const struct wl_client *trusted_client,
                            uint32_t max_radius, uint64_t max_region_pixels);
bool os_effect_manager_can_bind(const struct os_effect_manager *manager,
                                const struct wl_client *client);
struct os_effect_surface *
os_effect_manager_create_surface(struct os_effect_manager *manager,
                                 const struct wl_client *client,
                                 const void *surface);
bool os_effect_surface_set_blur(struct os_effect_manager *manager,
                                struct os_effect_surface *effect,
                                const struct os_effect_value *value);
bool os_effect_surface_commit(struct os_effect_surface *effect);
void os_effect_manager_destroy_surface(struct os_effect_manager *manager,
                                       const void *surface);
size_t os_effect_manager_surface_count(const struct os_effect_manager *manager);

#endif

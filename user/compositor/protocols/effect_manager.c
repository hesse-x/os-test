/*
 * Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#include "effect_manager.h"

#include <string.h>

void os_effect_manager_init(struct os_effect_manager *manager,
                            const struct wl_client *trusted_client,
                            uint32_t max_radius, uint64_t max_region_pixels) {
  if (manager == NULL)
    return;
  memset(manager, 0, sizeof(*manager));
  manager->max_radius = max_radius;
  manager->max_region_pixels = max_region_pixels;
  manager->next_id = 1;
  if (trusted_client != NULL)
    os_trusted_client_init(&manager->trusted, trusted_client);
}

bool os_effect_manager_can_bind(const struct os_effect_manager *manager,
                                const struct wl_client *client) {
  return manager != NULL &&
         os_trusted_client_matches(&manager->trusted, client);
}

struct os_effect_surface *
os_effect_manager_create_surface(struct os_effect_manager *manager,
                                 const struct wl_client *client,
                                 const void *surface) {
  if (!os_effect_manager_can_bind(manager, client) || surface == NULL)
    return NULL;
  struct os_effect_surface *available = NULL;
  for (size_t i = 0; i < OS_EFFECT_MAX_SURFACES; ++i) {
    if (manager->surfaces[i].occupied &&
        manager->surfaces[i].surface == surface)
      return NULL;
    if (!manager->surfaces[i].occupied && available == NULL)
      available = &manager->surfaces[i];
  }
  if (available == NULL)
    return NULL;
  *available = (struct os_effect_surface){
      .surface = surface, .id = manager->next_id++, .occupied = true};
  os_effect_state_init(&available->state);
  return available;
}

bool os_effect_surface_set_blur(struct os_effect_manager *manager,
                                struct os_effect_surface *effect,
                                const struct os_effect_value *value) {
  if (manager == NULL || effect == NULL || !effect->occupied || value == NULL)
    return false;
  uint64_t pixels = value->width > 0 && value->height > 0
                        ? (uint64_t)value->width * (uint64_t)value->height
                        : 0;
  return pixels <= manager->max_region_pixels &&
         os_effect_set_pending(&effect->state, value, manager->max_radius);
}

bool os_effect_surface_commit(struct os_effect_surface *effect) {
  return effect != NULL && effect->occupied &&
         os_effect_apply_surface_commit(&effect->state);
}

void os_effect_manager_destroy_surface(struct os_effect_manager *manager,
                                       const void *surface) {
  if (manager == NULL || surface == NULL)
    return;
  for (size_t i = 0; i < OS_EFFECT_MAX_SURFACES; ++i) {
    if (manager->surfaces[i].occupied &&
        manager->surfaces[i].surface == surface) {
      os_effect_discard_pending(&manager->surfaces[i].state);
      memset(&manager->surfaces[i], 0, sizeof(manager->surfaces[i]));
      return;
    }
  }
}

size_t
os_effect_manager_surface_count(const struct os_effect_manager *manager) {
  size_t count = 0;
  if (manager != NULL)
    for (size_t i = 0; i < OS_EFFECT_MAX_SURFACES; ++i)
      count += manager->surfaces[i].occupied;
  return count;
}

/*
 * Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#include "effect_server.h"

#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>

#include "effect_manager.h"
#include "os-effect-v1-server-protocol.h"

struct os_effect_server {
  struct wl_global *global;
  struct os_effect_manager manager;
  struct wl_list effects;
};

struct effect_resource {
  struct wl_list link;
  struct os_effect_server *server;
  struct os_effect_surface *model;
  struct wlr_surface *surface;
  struct wl_resource *resource;
  struct wl_listener commit;
  struct wl_listener surface_destroy;
};

static void effect_resource_destroy(struct wl_resource *resource) {
  struct effect_resource *effect = wl_resource_get_user_data(resource);
  if (effect == NULL)
    return;
  wl_list_remove(&effect->commit.link);
  wl_list_remove(&effect->surface_destroy.link);
  wl_list_remove(&effect->link);
  os_effect_manager_destroy_surface(&effect->server->manager, effect->surface);
  free(effect);
}

static void effect_destroy_request(struct wl_client *client,
                                   struct wl_resource *resource) {
  (void)client;
  wl_resource_destroy(resource);
}

static struct os_effect_value pending_value(struct effect_resource *effect) {
  return effect->model->state.pending_dirty ? effect->model->state.pending
                                            : effect->model->state.current;
}

static void effect_set_blur(struct wl_client *client,
                            struct wl_resource *resource, int32_t x, int32_t y,
                            int32_t width, int32_t height, uint32_t radius) {
  (void)client;
  struct effect_resource *effect = wl_resource_get_user_data(resource);
  struct os_effect_value value = pending_value(effect);
  value.enabled = true;
  value.x = x;
  value.y = y;
  value.width = width;
  value.height = height;
  value.radius = radius;
  if (!os_effect_surface_set_blur(&effect->server->manager, effect->model,
                                  &value))
    wl_resource_post_error(resource, OS_SURFACE_EFFECT_V1_ERROR_INVALID_REGION,
                           "blur parameters exceed compositor budget");
}

static void effect_unset_blur(struct wl_client *client,
                              struct wl_resource *resource) {
  (void)client;
  struct effect_resource *effect = wl_resource_get_user_data(resource);
  struct os_effect_value value = pending_value(effect);
  value.enabled = false;
  value.radius = 0;
  value.x = value.y = value.width = value.height = 0;
  os_effect_surface_set_blur(&effect->server->manager, effect->model, &value);
}

static void effect_set_tint(struct wl_client *client,
                            struct wl_resource *resource, uint32_t rgba) {
  (void)client;
  struct effect_resource *effect = wl_resource_get_user_data(resource);
  struct os_effect_value value = pending_value(effect);
  value.tint_rgba8 = rgba;
  if (!os_effect_surface_set_blur(&effect->server->manager, effect->model,
                                  &value))
    wl_resource_post_error(resource, OS_SURFACE_EFFECT_V1_ERROR_BUDGET,
                           "tint update exceeds compositor budget");
}

static const struct os_surface_effect_v1_interface effect_implementation = {
    .destroy = effect_destroy_request,
    .set_blur = effect_set_blur,
    .unset_blur = effect_unset_blur,
    .set_tint = effect_set_tint,
};

static void surface_commit(struct wl_listener *listener, void *data) {
  struct effect_resource *effect = wl_container_of(listener, effect, commit);
  (void)data;
  os_effect_surface_commit(effect->model);
}

static void surface_destroy(struct wl_listener *listener, void *data) {
  struct effect_resource *effect =
      wl_container_of(listener, effect, surface_destroy);
  (void)data;
  wl_resource_destroy(effect->resource);
}

static void manager_get_surface_effect(struct wl_client *client,
                                       struct wl_resource *manager_resource,
                                       uint32_t id,
                                       struct wl_resource *surface_resource) {
  struct os_effect_server *server = wl_resource_get_user_data(manager_resource);
  struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
  struct os_effect_surface *model =
      os_effect_manager_create_surface(&server->manager, client, surface);
  if (model == NULL) {
    wl_resource_post_error(manager_resource, OS_EFFECT_MANAGER_V1_ERROR_BUDGET,
                           "effect surface budget exhausted or duplicate");
    return;
  }
  struct wl_resource *resource =
      wl_resource_create(client, &os_surface_effect_v1_interface,
                         wl_resource_get_version(manager_resource), id);
  struct effect_resource *effect = calloc(1, sizeof(*effect));
  if (resource == NULL || effect == NULL) {
    free(effect);
    if (resource != NULL)
      wl_resource_destroy(resource);
    os_effect_manager_destroy_surface(&server->manager, surface);
    wl_client_post_no_memory(client);
    return;
  }
  effect->server = server;
  effect->model = model;
  effect->surface = surface;
  effect->resource = resource;
  effect->commit.notify = surface_commit;
  wl_signal_add(&surface->events.commit, &effect->commit);
  effect->surface_destroy.notify = surface_destroy;
  wl_signal_add(&surface->events.destroy, &effect->surface_destroy);
  wl_list_insert(&server->effects, &effect->link);
  wl_resource_set_implementation(resource, &effect_implementation, effect,
                                 effect_resource_destroy);
}

static void manager_destroy_request(struct wl_client *client,
                                    struct wl_resource *resource) {
  (void)client;
  wl_resource_destroy(resource);
}

static const struct os_effect_manager_v1_interface manager_implementation = {
    .destroy = manager_destroy_request,
    .get_surface_effect = manager_get_surface_effect,
};

static void bind_manager(struct wl_client *client, void *data, uint32_t version,
                         uint32_t id) {
  struct os_effect_server *server = data;
  if (!os_effect_manager_can_bind(&server->manager, client)) {
    wl_client_post_implementation_error(client,
                                        "os_effect_manager_v1 is trusted-only");
    return;
  }
  struct wl_resource *resource =
      wl_resource_create(client, &os_effect_manager_v1_interface, version, id);
  if (resource == NULL) {
    wl_client_post_no_memory(client);
    return;
  }
  wl_resource_set_implementation(resource, &manager_implementation, server,
                                 NULL);
  os_effect_manager_v1_send_capabilities(resource, server->manager.max_radius,
                                         OS_EFFECT_MAX_SURFACES);
}

struct os_effect_server *os_effect_server_create(struct wl_display *display) {
  if (display == NULL)
    return NULL;
  struct os_effect_server *server = calloc(1, sizeof(*server));
  if (server == NULL)
    return NULL;
  os_effect_manager_init(&server->manager, NULL, 32, 1280u * 720u);
  wl_list_init(&server->effects);
  server->global = wl_global_create(display, &os_effect_manager_v1_interface, 1,
                                    server, bind_manager);
  if (server->global == NULL) {
    free(server);
    return NULL;
  }
  return server;
}

bool os_effect_server_set_trusted_client(struct os_effect_server *server,
                                         struct wl_client *client) {
  return server != NULL &&
         os_trusted_client_init(&server->manager.trusted, client);
}

void os_effect_server_destroy(struct os_effect_server *server) {
  if (server == NULL)
    return;
  struct effect_resource *effect, *temporary;
  wl_list_for_each_safe(effect, temporary, &server->effects, link)
      wl_resource_destroy(effect->resource);
  wl_global_destroy(server->global);
  free(server);
}

bool os_effect_server_current(struct os_effect_server *server,
                              const struct wlr_surface *surface,
                              struct os_effect_value *value) {
  if (server == NULL || surface == NULL || value == NULL)
    return false;
  struct effect_resource *effect;
  wl_list_for_each(effect, &server->effects, link) {
    if (effect->surface == surface) {
      *value = effect->model->state.current;
      return true;
    }
  }
  return false;
}

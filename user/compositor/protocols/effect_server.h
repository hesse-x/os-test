/*
 * Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#ifndef OS_COMPOSITOR_EFFECT_SERVER_H
#define OS_COMPOSITOR_EFFECT_SERVER_H

#include <stdbool.h>

struct os_effect_server;
struct wl_client;
struct wl_display;
struct wlr_surface;
struct os_effect_value;

struct os_effect_server *os_effect_server_create(struct wl_display *display);
bool os_effect_server_set_trusted_client(struct os_effect_server *server,
                                         struct wl_client *client);
void os_effect_server_destroy(struct os_effect_server *server);
bool os_effect_server_current(struct os_effect_server *server,
                              const struct wlr_surface *surface,
                              struct os_effect_value *value);

#endif

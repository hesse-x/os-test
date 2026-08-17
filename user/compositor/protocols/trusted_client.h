/*
 * Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#ifndef OS_COMPOSITOR_TRUSTED_CLIENT_H
#define OS_COMPOSITOR_TRUSTED_CLIENT_H

#include <stdbool.h>

struct wl_client;

// Only the wl_client created from the inherited WAYLAND_SOCKET may bind the
// private effect global. Pointer identity is stable for the client lifetime.
struct os_trusted_client {
  const struct wl_client *wayland_socket_client;
};

bool os_trusted_client_init(struct os_trusted_client *trusted,
                            const struct wl_client *client);
bool os_trusted_client_matches(const struct os_trusted_client *trusted,
                               const struct wl_client *client);

#endif

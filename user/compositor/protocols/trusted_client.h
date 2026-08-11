/* Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#ifndef OS_COMPOSITOR_TRUSTED_CLIENT_H
#define OS_COMPOSITOR_TRUSTED_CLIENT_H

#include <stdbool.h>

struct wl_client;

/* Phase 1 stores only the wl_client created for the inherited WAYLAND_SOCKET.
 * The effect global remains unpublished until the protocol implementation is
 * enabled in phase 3. */
struct os_trusted_client {
  const struct wl_client *wayland_socket_client;
};

bool os_trusted_client_init(struct os_trusted_client *trusted,
                            const struct wl_client *client);
bool os_trusted_client_matches(const struct os_trusted_client *trusted,
                               const struct wl_client *client);

#endif

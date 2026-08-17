/*
 * Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#include "trusted_client.h"

#include <stddef.h>

bool os_trusted_client_init(struct os_trusted_client *trusted,
                            const struct wl_client *client) {
  if (trusted == NULL || client == NULL ||
      trusted->wayland_socket_client != NULL)
    return false;
  trusted->wayland_socket_client = client;
  return true;
}

bool os_trusted_client_matches(const struct os_trusted_client *trusted,
                               const struct wl_client *client) {
  return trusted != NULL && client != NULL &&
         trusted->wayland_socket_client == client;
}

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <spawn.h>

int posix_spawnattr_setflags(posix_spawnattr_t *attr, short flags) {
  const unsigned int all_flags =
      POSIX_SPAWN_RESETIDS | POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF |
      POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSCHEDPARAM |
      POSIX_SPAWN_SETSCHEDULER | POSIX_SPAWN_USEVFORK | POSIX_SPAWN_SETSID;
  const unsigned int scheduling_flags =
      POSIX_SPAWN_SETSCHEDPARAM | POSIX_SPAWN_SETSCHEDULER;
  const unsigned int requested = (unsigned short)flags;

  if (requested & ~all_flags)
    return EINVAL;
  if (requested & scheduling_flags)
    return ENOTSUP;

  attr->__flags = flags;
  return 0;
}

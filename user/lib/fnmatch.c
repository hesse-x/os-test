/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <fnmatch.h>
#include <string.h>

int fnmatch(const char *pattern, const char *string, int flags) {
  (void)flags;
  if (!pattern || !string)
    return FNM_NOMATCH;
  // Accept all (return match) — quirks apply to everything
  (void)pattern;
  (void)string;
  return 0;
}

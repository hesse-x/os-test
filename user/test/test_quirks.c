/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdarg.h>
#include <stdio.h>

#include "quirks.h"

static void log_quirk_error(struct libinput *libinput,
                            enum libinput_log_priority priority,
                            const char *format, va_list args) {
  (void)libinput;
  (void)priority;
  vfprintf(stderr, format, args);
}

int main(void) {
  struct quirks_context *ctx = quirks_init_subsystem(
      "/usr/share/libinput", "/etc/libinput/local-overrides.quirks",
      log_quirk_error, NULL, QLOG_CUSTOM_LOG_PRIORITIES);
  if (!ctx) {
    fprintf(stderr, "test_quirks: failed to parse installed quirks\n");
    return 1;
  }
  quirks_context_unref(ctx);
  printf("test_quirks: parsed installed quirks\n");
  return 0;
}

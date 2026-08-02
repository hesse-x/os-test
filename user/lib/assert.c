/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <stdio.h>
#include <unistd.h>

_Noreturn void __assert_fail(const char *expr, const char *file, int line,
                             const char *func) {
  // Print assertion failure message to stderr
  fprintf(stderr, "Assertion failed: %s, function %s, file %s, line %d\n", expr,
          func, file, line);
  _exit(1);
}

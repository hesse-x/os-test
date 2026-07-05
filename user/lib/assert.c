/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <stdio.h>
#include <unistd.h>

void __assert_fail(const char *expr, const char *file, int line) {
  // Print assertion failure message to stderr
  fprintf(stderr, "Assertion failed: %s, file %s, line %d\n", expr, file, line);
  _exit(1);
}

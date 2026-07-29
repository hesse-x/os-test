/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * __uflow stub for musl's string-scanning path.
 *
 * musl's strtol/strtod (src/stdlib/strtol.c / strtod.c) drive the integer/float
 * scanners in src/internal/{intscan,floatscan}.c, which read input through the
 * shgetc.h macros. The `shgetc(f)` macro is:
 *   ((f)->rpos < (f)->shend) ? *(f)->rpos++ : __shgetc(f)
 * and __shgetc (src/internal/shgetc.c) calls __uflow(f) when the scanner runs
 * past shend. For string scanning, strtol/strtod set up the fake FILE with
 * rend = (size_t)-1 (strtod) or rend = -1/2 (strtol), so shlim sets shend to
 * that huge sentinel; rpos only ever walks the actual string bytes and never
 * reaches shend. __shgetc is therefore never invoked at runtime — but the
 * macro still emits a link-time reference to __shgetc, which in turn references
 * __uflow. This stub provides __uflow to satisfy that reference; it returns EOF
 * and is unreachable.
 *
 * Compiled via add_musl_lib (musl_stdlib_objs) so it sees musl's private
 * stdio_impl.h — the FILE * matches musl's real layout, not the repo's.
 */

#include "stdio_impl.h"
#include <stdio.h>

int __uflow(FILE *f) {
  (void)f;
  return EOF;
}

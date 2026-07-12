/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * sincos — GNU libc extension, not part of standard C math.h.
 * Used by libinput internal code and some third-party code.
 */

#include <math.h>

/* Most glibc sincos implementations use a combined sin+cos
 * algorithm for efficiency. Here we call the separate builtins,
 * which GCC may still fuse where possible at -O2. */
void sincos(double x, double *sin_val, double *cos_val) {
  *sin_val = __builtin_sin(x);
  *cos_val = __builtin_cos(x);
}

void sincosf(float x, float *sin_val, float *cos_val) {
  *sin_val = __builtin_sinf(x);
  *cos_val = __builtin_cosf(x);
}

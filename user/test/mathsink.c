/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// mathsink — end-to-end smoke for musl libm (sqrt/sin/exp/copysignl/fmodl/
// sincos). Repo printf has no %f, so scale to integers for display. Ships as
// /usr/bin/mathsink; run from the shell. Returns 0 on PASS.
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>

int main(void) {
  double s = sqrt(2.0);                    // ~1.41421356
  double e = exp(1.0);                     // ~2.71828183
  double si = sin(3.14159265358979 / 2.0); // ~1.0
  long double cl = copysignl(3.0L, -1.0L); // -3
  long double ml = fmodl(5.0L, 3.0L);      // 2
  double sn, cs;
  sincos(0.0, &sn, &cs); // sn=0, cs=1

  printf("sqrt2*1e6=%ld exp1*1e6=%ld sinpi/2*1e6=%ld copysignl=%ld fmodl=%ld "
         "sincos0=%ld/%ld\n",
         (long)(s * 1e6), (long)(e * 1e6), (long)(si * 1e6), (long)cl, (long)ml,
         (long)sn, (long)cs);

  int ok = (s > 1.41421 && s < 1.41422) && (e > 2.71828 && e < 2.71829) &&
           (si > 0.99999 && si < 1.00001) && (cl == -3) && (ml == 2) &&
           (sn == 0) && (cs == 1);
  printf("math smoke: %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}

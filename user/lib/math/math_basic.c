/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * libm basic double-precision math functions.
 * All implemented via GCC __builtin_* — no hand-written numerical algorithms.
 * GCC x86-64 emits fsin/fcos/fsqrt/sqrtsd/etc. for these builtins.
 */

#include <math.h>

double acos(double x) { return __builtin_acos(x); }
double acosh(double x) { return __builtin_acosh(x); }
double asin(double x) { return __builtin_asin(x); }
double asinh(double x) { return __builtin_asinh(x); }
double atan(double x) { return __builtin_atan(x); }
double atanh(double x) { return __builtin_atanh(x); }
double atan2(double y, double x) { return __builtin_atan2(y, x); }
double cbrt(double x) { return __builtin_cbrt(x); }
double ceil(double x) { return __builtin_ceil(x); }
double copysign(double x, double y) { return __builtin_copysign(x, y); }
double cos(double x) { return __builtin_cos(x); }
double cosh(double x) { return __builtin_cosh(x); }
double erf(double x) { return __builtin_erf(x); }
double erfc(double x) { return __builtin_erfc(x); }
double exp(double x) { return __builtin_exp(x); }
double exp2(double x) { return __builtin_exp2(x); }
double expm1(double x) { return __builtin_expm1(x); }
double fabs(double x) { return __builtin_fabs(x); }
double fdim(double x, double y) { return __builtin_fdim(x, y); }
double floor(double x) { return __builtin_floor(x); }
double fma(double x, double y, double z) { return __builtin_fma(x, y, z); }
double fmax(double x, double y) { return __builtin_fmax(x, y); }
double fmin(double x, double y) { return __builtin_fmin(x, y); }
double fmod(double x, double y) { return __builtin_fmod(x, y); }
double frexp(double x, int *exp) { return __builtin_frexp(x, exp); }
double hypot(double x, double y) { return __builtin_hypot(x, y); }
double ldexp(double x, int exp) { return __builtin_ldexp(x, exp); }
double lgamma(double x) { return __builtin_lgamma(x); }
long long llrint(double x) { return __builtin_llrint(x); }
long long llround(double x) { return __builtin_llround(x); }
long lrint(double x) { return __builtin_lrint(x); }
long lround(double x) { return __builtin_lround(x); }
double log(double x) { return __builtin_log(x); }
double log10(double x) { return __builtin_log10(x); }
double log1p(double x) { return __builtin_log1p(x); }
double log2(double x) { return __builtin_log2(x); }
double logb(double x) { return __builtin_logb(x); }
double modf(double x, double *iptr) { return __builtin_modf(x, iptr); }
double nan(const char *tagp) { return __builtin_nan(tagp); }
double nearbyint(double x) { return __builtin_nearbyint(x); }
double pow(double x, double y) { return __builtin_pow(x, y); }
double remainder(double x, double y) { return __builtin_remainder(x, y); }
double remquo(double x, double y, int *q) { return __builtin_remquo(x, y, q); }
double rint(double x) { return __builtin_rint(x); }
double round(double x) { return __builtin_round(x); }
double scalbln(double x, long n) { return __builtin_scalbln(x, n); }
double scalbn(double x, int n) { return __builtin_scalbn(x, n); }
double sin(double x) { return __builtin_sin(x); }
double sinh(double x) { return __builtin_sinh(x); }
double sqrt(double x) { return __builtin_sqrt(x); }
double tan(double x) { return __builtin_tan(x); }
double tanh(double x) { return __builtin_tanh(x); }
double tgamma(double x) { return __builtin_tgamma(x); }
double trunc(double x) { return __builtin_trunc(x); }

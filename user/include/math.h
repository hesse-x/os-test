/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _MATH_H
#define _MATH_H

#define NAN (__builtin_nanf(""))
#define INFINITY (__builtin_inff())

#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_NORMAL 3
#define FP_SUBNORMAL 4

#define fpclassify(x)                                                          \
  __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)
#define isfinite(x) __builtin_isfinite(x)
#define isnormal(x) __builtin_isnormal(x)
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define signbit(x) __builtin_signbit(x)

#define M_E 2.71828182845904523536
#define M_LOG2E 1.44269504088896340736
#define M_LOG10E 0.43429448190325182765
#define M_LN2 0.69314718055994530942
#define M_LN10 2.30258509299404568402
#define M_PI 3.14159265358979323846
#define M_PI_2 1.57079632679489661923
#define M_PI_4 0.78539816339744830962
#define M_1_PI 0.31830988618379067154
#define M_2_PI 0.63661977236758134308
#define M_2_SQRTPI 1.12837916709551257390
#define M_SQRT2 1.41421356237309504880
#define M_SQRT1_2 0.70710678118654752440

#define HUGE_VALF __builtin_huge_valf()
#define HUGE_VAL __builtin_huge_val()
#define HUGE_VALL __builtin_huge_vall()

/* Out-of-line definitions in libm use __LIBM_BUILD__ to suppress
 * these static inline wrappers and provide real function symbols. */
#ifndef __LIBM_BUILD__

static inline double acos(double x) { return __builtin_acos(x); }
static inline double acosh(double x) { return __builtin_acosh(x); }
static inline double asin(double x) { return __builtin_asin(x); }
static inline double asinh(double x) { return __builtin_asinh(x); }
static inline double atan(double x) { return __builtin_atan(x); }
static inline double atanh(double x) { return __builtin_atanh(x); }
static inline double atan2(double y, double x) { return __builtin_atan2(y, x); }
static inline double cbrt(double x) { return __builtin_cbrt(x); }
static inline double ceil(double x) { return __builtin_ceil(x); }
static inline double copysign(double x, double y) {
  return __builtin_copysign(x, y);
}
static inline double cos(double x) { return __builtin_cos(x); }
static inline double cosh(double x) { return __builtin_cosh(x); }
static inline double erf(double x) { return __builtin_erf(x); }
static inline double erfc(double x) { return __builtin_erfc(x); }
static inline double exp(double x) { return __builtin_exp(x); }
static inline double exp2(double x) { return __builtin_exp2(x); }
static inline double expm1(double x) { return __builtin_expm1(x); }
static inline double fabs(double x) { return __builtin_fabs(x); }
static inline double fdim(double x, double y) { return __builtin_fdim(x, y); }
static inline double floor(double x) { return __builtin_floor(x); }
static inline double fma(double x, double y, double z) {
  return __builtin_fma(x, y, z);
}
static inline double fmax(double x, double y) { return __builtin_fmax(x, y); }
static inline double fmin(double x, double y) { return __builtin_fmin(x, y); }
static inline double fmod(double x, double y) { return __builtin_fmod(x, y); }
static inline double frexp(double x, int *exp) {
  return __builtin_frexp(x, exp);
}
static inline double hypot(double x, double y) { return __builtin_hypot(x, y); }
static inline int ilogb(double x) { return __builtin_ilogb(x); }
static inline double ldexp(double x, int exp) {
  return __builtin_ldexp(x, exp);
}
static inline double lgamma(double x) { return __builtin_lgamma(x); }
static inline long long llrint(double x) { return __builtin_llrint(x); }
static inline long long llround(double x) { return __builtin_llround(x); }
static inline double log(double x) { return __builtin_log(x); }
static inline double log10(double x) { return __builtin_log10(x); }
static inline double log1p(double x) { return __builtin_log1p(x); }
static inline double log2(double x) { return __builtin_log2(x); }
static inline double logb(double x) { return __builtin_logb(x); }
static inline long lrint(double x) { return __builtin_lrint(x); }
static inline long lround(double x) { return __builtin_lround(x); }
static inline double modf(double x, double *iptr) {
  return __builtin_modf(x, iptr);
}
static inline double nan(const char *tagp) { return __builtin_nan(tagp); }
static inline double nearbyint(double x) { return __builtin_nearbyint(x); }
static inline double pow(double x, double y) { return __builtin_pow(x, y); }
static inline double remainder(double x, double y) {
  return __builtin_remainder(x, y);
}
static inline double remquo(double x, double y, int *q) {
  return __builtin_remquo(x, y, q);
}
static inline double rint(double x) { return __builtin_rint(x); }
static inline double round(double x) { return __builtin_round(x); }
static inline double scalbln(double x, long n) {
  return __builtin_scalbln(x, n);
}
static inline double scalbn(double x, int n) { return __builtin_scalbn(x, n); }
static inline double sin(double x) { return __builtin_sin(x); }
static inline double sinh(double x) { return __builtin_sinh(x); }
static inline double sqrt(double x) { return __builtin_sqrt(x); }
static inline double tan(double x) { return __builtin_tan(x); }
static inline double tanh(double x) { return __builtin_tanh(x); }
static inline double tgamma(double x) { return __builtin_tgamma(x); }
static inline double trunc(double x) { return __builtin_trunc(x); }

static inline float acosf(float x) { return __builtin_acosf(x); }
static inline float acoshf(float x) { return __builtin_acoshf(x); }
static inline float asinf(float x) { return __builtin_asinf(x); }
static inline float asinhf(float x) { return __builtin_asinhf(x); }
static inline float atanf(float x) { return __builtin_atanf(x); }
static inline float atanhf(float x) { return __builtin_atanhf(x); }
static inline float atan2f(float y, float x) { return __builtin_atan2f(y, x); }
static inline float cbrtf(float x) { return __builtin_cbrtf(x); }
static inline float ceilf(float x) { return __builtin_ceilf(x); }
static inline float cosf(float x) { return __builtin_cosf(x); }
static inline float coshf(float x) { return __builtin_coshf(x); }
static inline float erff(float x) { return __builtin_erff(x); }
static inline float erfcf(float x) { return __builtin_erfcf(x); }
static inline float expf(float x) { return __builtin_expf(x); }
static inline float exp2f(float x) { return __builtin_exp2f(x); }
static inline float expm1f(float x) { return __builtin_expm1f(x); }
static inline float fabsf(float x) { return __builtin_fabsf(x); }
static inline float floorf(float x) { return __builtin_floorf(x); }
static inline float fmodf(float x, float y) { return __builtin_fmodf(x, y); }
static inline float hypotf(float x, float y) { return __builtin_hypotf(x, y); }
static inline float logf(float x) { return __builtin_logf(x); }
static inline float log10f(float x) { return __builtin_log10f(x); }
static inline float log2f(float x) { return __builtin_log2f(x); }
static inline float modff(float x, float *iptr) {
  return __builtin_modff(x, iptr);
}
static inline float powf(float x, float y) { return __builtin_powf(x, y); }
static inline float roundf(float x) { return __builtin_roundf(x); }
static inline float sinf(float x) { return __builtin_sinf(x); }
static inline float sinhf(float x) { return __builtin_sinhf(x); }
static inline float sqrtf(float x) { return __builtin_sqrtf(x); }
static inline float tanf(float x) { return __builtin_tanf(x); }
static inline float tanhf(float x) { return __builtin_tanhf(x); }
static inline float truncf(float x) { return __builtin_truncf(x); }

#endif /* __LIBM_BUILD__ */

#endif

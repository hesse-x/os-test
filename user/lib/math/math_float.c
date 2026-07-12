/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * libm single-precision math functions.
 * Same pattern: __builtin_* wrappers.
 */

#include <math.h>

float acosf(float x) { return __builtin_acosf(x); }
float acoshf(float x) { return __builtin_acoshf(x); }
float asinf(float x) { return __builtin_asinf(x); }
float asinhf(float x) { return __builtin_asinhf(x); }
float atanf(float x) { return __builtin_atanf(x); }
float atanhf(float x) { return __builtin_atanhf(x); }
float atan2f(float y, float x) { return __builtin_atan2f(y, x); }
float cbrtf(float x) { return __builtin_cbrtf(x); }
float ceilf(float x) { return __builtin_ceilf(x); }
float cosf(float x) { return __builtin_cosf(x); }
float coshf(float x) { return __builtin_coshf(x); }
float erff(float x) { return __builtin_erff(x); }
float erfcf(float x) { return __builtin_erfcf(x); }
float expf(float x) { return __builtin_expf(x); }
float exp2f(float x) { return __builtin_exp2f(x); }
float expm1f(float x) { return __builtin_expm1f(x); }
float fabsf(float x) { return __builtin_fabsf(x); }
float floorf(float x) { return __builtin_floorf(x); }
float fmodf(float x, float y) { return __builtin_fmodf(x, y); }
float hypotf(float x, float y) { return __builtin_hypotf(x, y); }
float logf(float x) { return __builtin_logf(x); }
float log10f(float x) { return __builtin_log10f(x); }
float log2f(float x) { return __builtin_log2f(x); }
float modff(float x, float *iptr) { return __builtin_modff(x, iptr); }
float powf(float x, float y) { return __builtin_powf(x, y); }
float roundf(float x) { return __builtin_roundf(x); }
float sinf(float x) { return __builtin_sinf(x); }
float sinhf(float x) { return __builtin_sinhf(x); }
float sqrtf(float x) { return __builtin_sqrtf(x); }
float tanf(float x) { return __builtin_tanf(x); }
float tanhf(float x) { return __builtin_tanhf(x); }
float truncf(float x) { return __builtin_truncf(x); }

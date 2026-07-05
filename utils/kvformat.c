/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "utils/kvformat.h"
#include <stdint.h>

/* Helper: emit unsigned integer in given base */
static int kvfmt_uint(void (*putc)(char c, void *arg), void *arg,
                      unsigned long val, int base, int uppercase, int width,
                      char pad, int left_align) {
  const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
  char buf[20];
  int pos = 0;

  if (val == 0) {
    buf[pos++] = '0';
  } else {
    while (val) {
      buf[pos++] = digits[val % base];
      val /= base;
    }
  }

  int ndigits = pos;

  if (left_align) {
    while (--pos >= 0)
      putc(buf[pos], arg);
    for (int i = ndigits; i < width; i++)
      putc(' ', arg);
  } else {
    while (pos < width)
      buf[pos++] = pad;
    while (--pos >= 0)
      putc(buf[pos], arg);
  }
  return ndigits > width ? ndigits : width;
}

/* Helper: emit signed integer — correctly handles space-padded negatives */
static int kvfmt_int(void (*putc)(char c, void *arg), void *arg, long val,
                     int width, char pad, int left_align) {
  unsigned long uval;
  int neg = 0;

  if (val < 0) {
    neg = 1;
    uval = -(unsigned long)val; /* well-defined for all negative long */
  } else {
    uval = (unsigned long)val;
  }

  if (!neg)
    return kvfmt_uint(putc, arg, uval, 10, 0, width, pad, left_align);

  /* Count decimal digits of |val| */
  unsigned long tmp = uval;
  int ndigits = 1;
  while (tmp >= 10) {
    ndigits++;
    tmp /= 10;
  }

  int total = ndigits + 1; /* digits + '-' sign */
  int pcount = width > total ? width - total : 0;

  if (left_align) {
    /* minus, digits, then spaces on the right */
    putc('-', arg);
    int r = 1 + kvfmt_uint(putc, arg, uval, 10, 0, 0, ' ', 0);
    for (int i = 0; i < pcount; i++) {
      putc(' ', arg);
      r++;
    }
    return r;
  } else if (pad == '0') {
    /* zero-fill: minus, then zeros, then digits */
    putc('-', arg);
    return 1 + kvfmt_uint(putc, arg, uval, 10, 0, ndigits + pcount, '0', 0);
  } else {
    /* space-pad: spaces on the left, then minus, then digits */
    for (int i = 0; i < pcount; i++)
      putc(' ', arg);
    putc('-', arg);
    return pcount + 1 + kvfmt_uint(putc, arg, uval, 10, 0, 0, ' ', 0);
  }
}

/* ===================== floating-point (%f) =====================
 * Freestanding, no <math.h> dependency. Handles sign, integer/fraction
 * split, precision-driven rounding (round-half-up), and the special
 * values nan / +inf / -inf. Width + pad + left_align reuse the same
 * semantics as integer specifiers. Default precision is 6.
 *
 * User-space only. The kernel is built with -mno-sse, and the x86-64
 * ABI requires SSE (XMM) for double arguments/returns — so a `double`
 * in the signature, or even va_arg(ap, double), would fail to compile.
 * The %f branch in kvformat() is likewise excluded under __KERNEL__.
 */
#ifndef __KERNEL__
static int kvfmt_emit_str(void (*putc)(char c, void *arg), void *arg,
                          const char *s) {
  int n = 0;
  while (*s) {
    putc(*s++, arg);
    n++;
  }
  return n;
}

static int kvfmt_double(void (*putc)(char c, void *arg), void *arg,
                        uint64_t bits, int width, char pad, int left_align,
                        int precision) {
  if (precision < 0)
    precision = 6;
  if (precision > 60)
    precision = 60; /* cap to fbuf[] size */

  int exp_bits = (int)((bits >> 52) & 0x7FF);
  uint64_t man = bits & 0xFFFFFFFFFFFFFULL;
  int sign = (int)(bits >> 63) & 1;

  if (exp_bits == 0x7FF) {
    const char *s = man ? "nan" : (sign ? "-inf" : "inf");
    int slen = 0;
    while (s[slen])
      slen++;
    if (left_align) {
      int n = kvfmt_emit_str(putc, arg, s);
      for (int i = slen; i < width; i++) {
        putc(' ', arg);
        n++;
      }
      return n;
    }
    for (int i = slen; i < width; i++)
      putc(pad == '0' ? ' ' : pad, arg);
    return (width > slen ? width : slen) - slen + kvfmt_emit_str(putc, arg, s);
  }

  /* Reconstruct the double; arithmetic stays in local registers so the
   * -mno-sse kernel build uses x87 and never touches XMM. */
  double val;
  __builtin_memcpy(&val, &bits, sizeof(val));

  int neg = sign && val < 0;
  if (val < 0)
    val = -val;

  /* Round at the requested precision: add 0.5 * 10^(-precision). */
  double round_unit = 0.5;
  for (int i = 0; i < precision; i++)
    round_unit *= 0.1;
  val += round_unit;

  uint64_t int_part = (uint64_t)val;
  double frac = val - (double)int_part;

  /* Render integer part. */
  char ibuf[24];
  int ipos = 0;
  if (int_part == 0) {
    ibuf[ipos++] = '0';
  } else {
    while (int_part) {
      ibuf[ipos++] = '0' + (int)(int_part % 10);
      int_part /= 10;
    }
  }

  /* Render fractional digits into fbuf (precision digits). */
  char fbuf[64];
  for (int i = 0; i < precision; i++) {
    frac *= 10.0;
    int d = (int)frac;
    if (d < 0)
      d = 0;
    if (d > 9)
      d = 9;
    fbuf[i] = '0' + d;
    frac -= d;
  }

  /* Total = sign + integer digits + (precision>0 ? 1 + precision : 0). */
  int int_len = ipos;
  int frac_len = precision > 0 ? 1 + precision : 0;
  int total = (neg ? 1 : 0) + int_len + frac_len;
  int pad_count = width > total ? width - total : 0;

  int emitted = 0;
  if (left_align) {
    if (neg) {
      putc('-', arg);
      emitted++;
    }
    for (int i = ipos - 1; i >= 0; i--) {
      putc(ibuf[i], arg);
      emitted++;
    }
    if (precision > 0) {
      putc('.', arg);
      emitted++;
      for (int i = 0; i < precision; i++) {
        putc(fbuf[i], arg);
        emitted++;
      }
    }
    for (int i = 0; i < pad_count; i++) {
      putc(' ', arg);
      emitted++;
    }
  } else if (pad == '0') {
    if (neg) {
      putc('-', arg);
      emitted++;
    }
    for (int i = 0; i < pad_count; i++) {
      putc('0', arg);
      emitted++;
    }
    for (int i = ipos - 1; i >= 0; i--) {
      putc(ibuf[i], arg);
      emitted++;
    }
    if (precision > 0) {
      putc('.', arg);
      emitted++;
      for (int i = 0; i < precision; i++) {
        putc(fbuf[i], arg);
        emitted++;
      }
    }
  } else {
    for (int i = 0; i < pad_count; i++) {
      putc(' ', arg);
      emitted++;
    }
    if (neg) {
      putc('-', arg);
      emitted++;
    }
    for (int i = ipos - 1; i >= 0; i--) {
      putc(ibuf[i], arg);
      emitted++;
    }
    if (precision > 0) {
      putc('.', arg);
      emitted++;
      for (int i = 0; i < precision; i++) {
        putc(fbuf[i], arg);
        emitted++;
      }
    }
  }
  return emitted;
}
#endif /* !__KERNEL__ */

int kvformat(void (*putc)(char c, void *arg), void *arg, const char *fmt,
             va_list ap) {
  int count = 0;

  while (*fmt) {
    if (*fmt != '%') {
      putc(*fmt++, arg);
      count++;
      continue;
    }

    fmt++; /* skip '%' */

    /* --- flags --- */
    int left_align = 0;
    if (*fmt == '-') {
      left_align = 1;
      fmt++;
    }

    /* --- width and pad character --- */
    int width = 0;
    char pad = ' ';
    if (*fmt == '0' && !left_align) {
      pad = '0';
      fmt++;
    }
    while (*fmt >= '0' && *fmt <= '9') {
      width = width * 10 + (*fmt - '0');
      fmt++;
    }

    /* --- precision --- */
    int precision = -1;
    if (*fmt == '.') {
      fmt++;
      precision = 0;
      while (*fmt >= '0' && *fmt <= '9') {
        precision = precision * 10 + (*fmt - '0');
        fmt++;
      }
    }

    /* --- length modifier --- */
    int is_long = 0;
    int is_long_long = 0;
    int is_size = 0;
    if (*fmt == 'l') {
      is_long = 1;
      fmt++;
      if (*fmt == 'l') {
        is_long_long = 1;
        fmt++;
      }
    }
    if (*fmt == 'z') {
      is_size = 1;
      fmt++;
    }

    /* --- specifier --- */
    switch (*fmt) {
    case '%':
      putc('%', arg);
      count++;
      break;

    case 'c': {
      char c = (char)va_arg(ap, int);
      putc(c, arg);
      count++;
      break;
    }

    case 's': {
      const char *s = va_arg(ap, const char *);
      if (!s)
        s = "(null)";
      int slen = 0;
      while (s[slen])
        slen++;
      if (left_align) {
        for (int i = 0; i < slen; i++)
          putc(s[i], arg);
        for (int i = slen; i < width; i++)
          putc(' ', arg);
      } else {
        for (int i = slen; i < width; i++)
          putc(' ', arg);
        for (int i = 0; i < slen; i++)
          putc(s[i], arg);
      }
      count += slen > width ? slen : width;
      break;
    }

    case 'd': {
      long val;
      if (is_long_long)
        val = va_arg(ap, long long);
      else if (is_long || is_size)
        val = va_arg(ap, long);
      else
        val = (long)va_arg(ap, int);
      count += kvfmt_int(putc, arg, val, width, pad, left_align);
      break;
    }

    case 'u': {
      unsigned long val;
      if (is_long_long)
        val = va_arg(ap, unsigned long long);
      else if (is_long || is_size)
        val = va_arg(ap, unsigned long);
      else
        val = (unsigned long)va_arg(ap, unsigned int);
      count += kvfmt_uint(putc, arg, val, 10, 0, width, pad, left_align);
      break;
    }

    case 'o': {
      unsigned long val;
      if (is_long_long)
        val = va_arg(ap, unsigned long long);
      else if (is_long || is_size)
        val = va_arg(ap, unsigned long);
      else
        val = (unsigned long)va_arg(ap, unsigned int);
      count += kvfmt_uint(putc, arg, val, 8, 0, width, pad, left_align);
      break;
    }

    case 'x':
    case 'X': {
      int upper = (*fmt == 'X');
      unsigned long val;
      if (is_long_long)
        val = va_arg(ap, unsigned long long);
      else if (is_long || is_size)
        val = va_arg(ap, unsigned long);
      else
        val = (unsigned long)va_arg(ap, unsigned int);
      count += kvfmt_uint(putc, arg, val, 16, upper, width, pad, left_align);
      break;
    }

    case 'p': {
      unsigned long val = (unsigned long)va_arg(ap, void *);
      putc('0', arg);
      putc('x', arg);
      count += 2 + kvfmt_uint(putc, arg, val, 16, 0, 0, '0', 0);
      break;
    }

    case 'f':
    case 'F': {
      /* In C, float arguments are promoted to double in variadic
       * calls, so %f and %lf both consume a double. The 'l' length
       * modifier is a no-op for 'f' in printf.
       *
       * User-space only: the kernel is built with -mno-sse, and
       * va_arg(ap, double) itself requires the SSE ABI, so the
       * kernel build excludes this branch. */
#ifndef __KERNEL__
      (void)is_long;
      (void)is_long_long;
      (void)is_size;
      double val = va_arg(ap, double);
      uint64_t bits;
      __builtin_memcpy(&bits, &val, sizeof(bits));
      count += kvfmt_double(putc, arg, bits, width, pad, left_align, precision);
      break;
#else
      /* fall through to default for the kernel build */
#endif
    }

    default:
      /* unknown specifier: output % + char as-is */
      putc('%', arg);
      count++;
      if (is_long_long) {
        putc('l', arg);
        putc('l', arg);
        count += 2;
      } else if (is_long) {
        putc('l', arg);
        count++;
      }
      if (is_size) {
        putc('z', arg);
        count++;
      }
      putc(*fmt, arg);
      count++;
      break;
    }

    fmt++;
  }

  return count;
}

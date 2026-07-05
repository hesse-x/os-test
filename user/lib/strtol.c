/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

static int digit_val(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'Z')
    return c - 'A' + 10;
  return -1;
}

int atoi(const char *s) { return (int)strtol(s, (char **)NULL, 10); }

long atol(const char *s) { return strtol(s, (char **)NULL, 10); }

long long atoll(const char *s) { return strtoll(s, (char **)NULL, 10); }

long strtol(const char *s, char **endptr, int base) {
  const char *p = s;
  long result = 0;
  int sign = 1;

  // Skip whitespace
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    p++;

  // Handle sign
  if (*p == '-') {
    sign = -1;
    p++;
  } else if (*p == '+')
    p++;

  // Handle base detection
  if (base == 0) {
    if (*p == '0') {
      p++;
      if (*p == 'x' || *p == 'X') {
        base = 16;
        p++;
      } else {
        base = 8;
      }
    } else {
      base = 10;
    }
  } else if (base == 16) {
    if (*p == '0' && (*(p + 1) == 'x' || *(p + 1) == 'X'))
      p += 2;
  }

  // Parse digits
  while (*p) {
    int v = digit_val(*p);
    if (v < 0 || v >= base)
      break;
    result = result * base + v;
    p++;
  }

  if (endptr)
    *endptr = (char *)p;
  return sign * result;
}

unsigned long strtoul(const char *s, char **endptr, int base) {
  const char *p = s;
  unsigned long result = 0;

  // Skip whitespace
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    p++;

  // Handle sign
  if (*p == '+')
    p++;

  // Handle base
  if (base == 0) {
    if (*p == '0') {
      p++;
      if (*p == 'x' || *p == 'X') {
        base = 16;
        p++;
      } else {
        base = 8;
      }
    } else {
      base = 10;
    }
  } else if (base == 16) {
    if (*p == '0' && (*(p + 1) == 'x' || *(p + 1) == 'X'))
      p += 2;
  }

  while (*p) {
    int v = digit_val(*p);
    if (v < 0 || v >= base)
      break;
    result = result * base + v;
    p++;
  }

  if (endptr)
    *endptr = (char *)p;
  return result;
}

long long strtoll(const char *s, char **endptr, int base) {
  const char *p = s;
  long long result = 0;
  int sign = 1;

  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    p++;

  if (*p == '-') {
    sign = -1;
    p++;
  } else if (*p == '+')
    p++;

  if (base == 0) {
    if (*p == '0') {
      p++;
      if (*p == 'x' || *p == 'X') {
        base = 16;
        p++;
      } else {
        base = 8;
      }
    } else {
      base = 10;
    }
  } else if (base == 16) {
    if (*p == '0' && (*(p + 1) == 'x' || *(p + 1) == 'X'))
      p += 2;
  }

  while (*p) {
    int v = digit_val(*p);
    if (v < 0 || v >= base)
      break;
    result = result * base + v;
    p++;
  }

  if (endptr)
    *endptr = (char *)p;
  return sign * result;
}

unsigned long long strtoull(const char *s, char **endptr, int base) {
  const char *p = s;
  unsigned long long result = 0;

  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    p++;

  if (*p == '+')
    p++;

  if (base == 0) {
    if (*p == '0') {
      p++;
      if (*p == 'x' || *p == 'X') {
        base = 16;
        p++;
      } else {
        base = 8;
      }
    } else {
      base = 10;
    }
  } else if (base == 16) {
    if (*p == '0' && (*(p + 1) == 'x' || *(p + 1) == 'X'))
      p += 2;
  }

  while (*p) {
    int v = digit_val(*p);
    if (v < 0 || v >= base)
      break;
    result = result * base + v;
    p++;
  }

  if (endptr)
    *endptr = (char *)p;
  return result;
}

/* strtod / atof — 无 libm 的有限精度十进制解析（处理常规
 * [-]ddd[.ddd][e[+-]dd]） */
double strtod(const char *s, char **endptr) {
  const char *p = s;
  double result = 0.0;
  double sign = 1.0;
  int got_digit = 0;

  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    p++;

  if (*p == '-') {
    sign = -1.0;
    p++;
  } else if (*p == '+')
    p++;

  while (*p >= '0' && *p <= '9') {
    result = result * 10.0 + (*p - '0');
    got_digit = 1;
    p++;
  }

  if (*p == '.') {
    p++;
    double scale = 0.1;
    while (*p >= '0' && *p <= '9') {
      result += (*p - '0') * scale;
      scale *= 0.1;
      got_digit = 1;
      p++;
    }
  }

  if (got_digit && (*p == 'e' || *p == 'E')) {
    const char *e_start = p;
    p++;
    int esign = 1;
    if (*p == '-') {
      esign = -1;
      p++;
    } else if (*p == '+')
      p++;
    int exp = 0, exp_digits = 0;
    while (*p >= '0' && *p <= '9') {
      exp = exp * 10 + (*p - '0');
      exp_digits++;
      p++;
    }
    if (exp_digits == 0) {
      /* 无效指数，回退到 e 之前 */
      p = e_start;
    } else {
      double mul = 1.0;
      for (int i = 0; i < exp; i++)
        mul *= 10.0;
      if (esign > 0)
        result *= mul;
      else
        result /= mul;
    }
  }

  if (endptr)
    *endptr = (char *)p;
  return sign * result;
}

double atof(const char *s) { return strtod(s, (char **)NULL); }

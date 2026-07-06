/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>
#include <stdint.h>
#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 32767

/* div / ldiv / lldiv result types */
typedef struct {
  int quot;
  int rem;
} div_t;
typedef struct {
  long quot;
  long rem;
} ldiv_t;
typedef struct {
  long long quot;
  long long rem;
} lldiv_t;
typedef struct {
  intmax_t quot;
  intmax_t rem;
} imaxdiv_t;

LIBC_EXPORT void *malloc(size_t size);
LIBC_EXPORT void free(void *ptr);
LIBC_EXPORT void *calloc(size_t nmemb, size_t size);
LIBC_EXPORT void *realloc(void *ptr, size_t size);

LIBC_EXPORT void exit(int status);
LIBC_EXPORT void abort(void);
LIBC_EXPORT int atexit(void (*func)(void));
int on_exit(void (*func)(int, void *), void *arg);

LIBC_EXPORT int atoi(const char *s);
LIBC_EXPORT long atol(const char *s);
LIBC_EXPORT long long atoll(const char *s);
LIBC_EXPORT long strtol(const char *s, char **endptr, int base);
LIBC_EXPORT unsigned long strtoul(const char *s, char **endptr, int base);
LIBC_EXPORT long long strtoll(const char *s, char **endptr, int base);
LIBC_EXPORT unsigned long long strtoull(const char *s, char **endptr, int base);
LIBC_EXPORT double atof(const char *s);
LIBC_EXPORT double strtod(const char *s, char **endptr);
LIBC_EXPORT int abs(int x);
LIBC_EXPORT long labs(long x);
LIBC_EXPORT long long llabs(long long x);
LIBC_EXPORT intmax_t imaxabs(intmax_t x);
LIBC_EXPORT imaxdiv_t imaxdiv(intmax_t numer, intmax_t denom);
LIBC_EXPORT div_t div(int numer, int denom);
LIBC_EXPORT ldiv_t ldiv(long numer, long denom);
LIBC_EXPORT lldiv_t lldiv(long long numer, long long denom);
LIBC_EXPORT int rand(void);
LIBC_EXPORT int rand_r(unsigned *seedp);
LIBC_EXPORT void srand(unsigned seed);
LIBC_EXPORT void qsort(void *base, size_t nmemb, size_t size,
                       int (*cmp)(const void *, const void *));
LIBC_EXPORT void *bsearch(const void *key, const void *base, size_t nmemb,
                          size_t size, int (*cmp)(const void *, const void *));

/* Environment variables (environ.c) */
LIBC_EXPORT extern char **environ;
LIBC_EXPORT char *getenv(const char *name);
LIBC_EXPORT int setenv(const char *name, const char *value, int overwrite);
LIBC_EXPORT int putenv(char *string);
LIBC_EXPORT int unsetenv(const char *name);
LIBC_EXPORT int clearenv(void);

int system(const char *command);

/* Temp file + path canonicalization (group 3) */
LIBC_EXPORT int mkstemp(char *tmpl);
LIBC_EXPORT char *mktemp(char *tmpl);
LIBC_EXPORT char *realpath(const char *path, char *resolved);

#ifdef __cplusplus
}
#endif

#endif /* _STDLIB_H */
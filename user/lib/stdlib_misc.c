/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <xos/errno.h>
#include <xos/fcntl.h>

void exit(int status) {
  fflush(stdout);
  fflush(stderr);
  _exit(status);
}

static unsigned long next_rand = 1;

int rand(void) {
  next_rand = next_rand * 1103515245 + 12345;
  return (int)((next_rand / 65536) % 32768);
}

void srand(unsigned seed) { next_rand = seed; }

int rand_r(unsigned *seedp) {
  *seedp = *seedp * 1103515245 + 12345;
  return (int)((*seedp / 65536) % 32768);
}

int abs(int x) { return x < 0 ? -x : x; }

long labs(long x) { return x < 0 ? -x : x; }

long long llabs(long long x) { return x < 0 ? -x : x; }

intmax_t imaxabs(intmax_t x) { return x < 0 ? -x : x; }

imaxdiv_t imaxdiv(intmax_t numer, intmax_t denom) {
  imaxdiv_t r;
  r.quot = numer / denom;
  r.rem = numer % denom;
  return r;
}

div_t div(int numer, int denom) {
  div_t r;
  r.quot = numer / denom;
  r.rem = numer % denom;
  return r;
}

ldiv_t ldiv(long numer, long denom) {
  ldiv_t r;
  r.quot = numer / denom;
  r.rem = numer % denom;
  return r;
}

lldiv_t lldiv(long long numer, long long denom) {
  lldiv_t r;
  r.quot = numer / denom;
  r.rem = numer % denom;
  return r;
}

/* qsort — a real quicksort (median-of-three + insertion sort for small arrays).
 * The original implementation was pure insertion sort, O(n²) worst case. */
static void swap_bytes(char *a, char *b, size_t size) {
  while (size--) {
    char t = *a;
    *a++ = *b;
    *b++ = t;
  }
}

static void qsort_range(char *arr, size_t lo, size_t hi, size_t size,
                        int (*cmp)(const void *, const void *)) {
  while (lo < hi) {
    /* Use insertion sort for small arrays to avoid recursion overhead */
    if (hi - lo < 8) {
      for (size_t i = lo + 1; i <= hi; i++) {
        size_t j = i;
        while (j > lo && cmp(&arr[j * size], &arr[(j - 1) * size]) < 0) {
          swap_bytes(&arr[j * size], &arr[(j - 1) * size], size);
          j--;
        }
      }
      return;
    }
    /* median-of-three pivot selection, place pivot at hi */
    size_t mid = lo + (hi - lo) / 2;
    if (cmp(&arr[mid * size], &arr[hi * size]) > 0)
      swap_bytes(&arr[mid * size], &arr[hi * size], size);
    if (cmp(&arr[lo * size], &arr[hi * size]) > 0)
      swap_bytes(&arr[lo * size], &arr[hi * size], size);
    if (cmp(&arr[mid * size], &arr[lo * size]) > 0)
      swap_bytes(&arr[mid * size], &arr[lo * size], size);
    char *pivot = &arr[lo * size];
    size_t i = lo;
    for (size_t j = lo + 1; j <= hi; j++) {
      if (cmp(&arr[j * size], pivot) < 0) {
        i++;
        swap_bytes(&arr[i * size], &arr[j * size], size);
      }
    }
    swap_bytes(&arr[lo * size], &arr[i * size], size);
    /* Tail-call elimination: recurse on the shorter side, loop on the longer
     * side */
    if (i - lo < hi - i) {
      qsort_range(arr, lo, i - 1, size, cmp);
      lo = i + 1;
    } else {
      qsort_range(arr, i + 1, hi, size, cmp);
      hi = i - 1;
    }
  }
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*cmp)(const void *, const void *)) {
  if (nmemb <= 1)
    return;
  qsort_range((char *)base, 0, nmemb - 1, size, cmp);
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*cmp)(const void *, const void *)) {
  const char *arr = (const char *)base;
  size_t lo = 0, hi = nmemb;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int c = cmp(key, &arr[mid * size]);
    if (c == 0)
      return (void *)&arr[mid * size];
    if (c < 0)
      hi = mid;
    else
      lo = mid + 1;
  }
  return NULL;
}

/* ==================== mkstemp / mktemp (group 3) ====================
 * Replace the trailing X's in template with random letters, then open with
 * O_CREAT|O_EXCL|O_RDWR so a pre-existing name yields EEXIST and we retry.
 * Relies on the O_EXCL semantics enforced by sys_open (via i_op->create). */
static int fill_xxx(char *tmpl, int xstart, int xlen) {
  /* Seed from getpid + a monotonic counter so concurrent/sequential calls in
   * one process produce distinct names without requiring srand(). */
  static unsigned counter = 0;
  unsigned seed = (unsigned)getpid() * 2654435761u + (counter++ * 2246822519u);
  const char *set = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123"
                    "456789";
  for (int i = 0; i < xlen; i++) {
    seed = seed * 1103515245u + 12345u;
    tmpl[xstart + i] = set[(seed / 65536) % 62];
  }
  return 0;
}

/* Find the run of trailing X's in template (POSIX requires >=6). Returns the
 * start index and length via out params, or -1 if none. */
static int find_xrun(char *tmpl, int *start, int *len) {
  int slen = (int)strlen(tmpl);
  int i = slen;
  while (i > 0 && tmpl[i - 1] == 'X')
    i--;
  if (i == slen)
    return -1;
  *start = i;
  *len = slen - i;
  return 0;
}

int mkstemp(char *tmpl) {
  int start, len;
  if (find_xrun(tmpl, &start, &len) < 0 || len < 6) {
    errno = EINVAL;
    return -1;
  }
  /* Try up to 2^len distinct names (capped). */
  for (int attempt = 0; attempt < 256; attempt++) {
    fill_xxx(tmpl, start, len);
    int fd = open(tmpl, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd >= 0)
      return fd;
    if (errno != EEXIST)
      return -1;
  }
  errno = EEXIST;
  return -1;
}

/* mktemp: fill the template with a unique name that does not exist, without
 * opening it. Returns template on success, "" on failure. Inherently racy
 * (POSIX warns so); acceptable for this libc. */
char *mktemp(char *tmpl) {
  int start, len;
  if (find_xrun(tmpl, &start, &len) < 0 || len < 6) {
    tmpl[0] = '\0';
    return tmpl;
  }
  for (int attempt = 0; attempt < 256; attempt++) {
    fill_xxx(tmpl, start, len);
    struct stat st;
    if (stat(tmpl, &st) < 0) {
      if (errno == ENOENT)
        return tmpl; /* name is free */
      tmpl[0] = '\0';
      return tmpl;
    }
  }
  tmpl[0] = '\0';
  return tmpl;
}

/* ==================== realpath (group 3) ====================
 * No symlinks exist in this FS yet, so realpath reduces to: make the path
 * absolute (relative → getcwd join) then collapse . / .. / redundant slashes.
 * Returns resolved (or buf if non-NULL) on success, NULL on failure. */
char *realpath(const char *path, char *resolved) {
  if (!path || !path[0]) {
    errno = EINVAL;
    return NULL;
  }
  static char buf_storage[4096];
  char *buf = resolved ? resolved : buf_storage;

  char abs[4096];
  if (path[0] == '/') {
    strncpy(abs, path, sizeof(abs) - 1);
    abs[sizeof(abs) - 1] = '\0';
  } else {
    if (!getcwd(abs, sizeof(abs))) {
      errno = ENAMETOOLONG;
      return NULL;
    }
    size_t cl = strlen(abs);
    if (cl + 1 + strlen(path) + 1 > sizeof(abs)) {
      errno = ENAMETOOLONG;
      return NULL;
    }
    abs[cl++] = '/';
    strcpy(abs + cl, path);
  }

  /* Canonicalize: split on '/', drop empty + ".", apply "..". */
  char *out = buf;
  size_t outcap = 4096;
  /* Stack of component start offsets within buf. */
  int starts[256];
  int depth = 0;
  size_t o = 0;
  const char *p = abs;
  while (*p) {
    while (*p == '/')
      p++;
    if (!*p)
      break;
    const char *seg = p;
    while (*p && *p != '/')
      p++;
    size_t seglen = (size_t)(p - seg);
    if (seglen == 1 && seg[0] == '.')
      continue;
    if (seglen == 2 && seg[0] == '.' && seg[1] == '.') {
      if (depth > 0) {
        depth--;
        /* Pop the previous component: starts[depth] points just past its
         * leading '/', so rewind one further to drop that '/' too. This
         * prevents the next component from producing a doubled "//". */
        o = (size_t)starts[depth] - 1;
      }
      continue;
    }
    if (o + 1 >= outcap) {
      errno = ENAMETOOLONG;
      return NULL;
    }
    buf[o++] = '/';
    starts[depth++] = (int)o;
    if (o + seglen >= outcap) {
      errno = ENAMETOOLONG;
      return NULL;
    }
    memcpy(buf + o, seg, seglen);
    o += seglen;
  }
  if (o == 0) {
    buf[o++] = '/';
  }
  buf[o] = '\0';
  (void)out;
  return buf;
}

int mknod(const char *path, mode_t mode, dev_t dev) {
  (void)path;
  (void)mode;
  (void)dev;
  errno = ENOSYS;
  return -1;
}

/* POSIX functions referenced by upstream libdrm's device-enumeration and
 * node-creation paths (chown/chmod/remove/readlink/getline/sscanf/fscanf).
 * None lie on the drmOpen/drmModeGetResources path verified in plan_drm2
 * step 10; stubs keep the libdrm compile/link unit whole and return failure
 * (errno=ENOSYS) if reached, matching the mknod stub above. */
int chown(const char *path, uid_t owner, gid_t group) {
  (void)path;
  (void)owner;
  (void)group;
  errno = ENOSYS;
  return -1;
}

int chmod(const char *path, mode_t mode) {
  (void)path;
  (void)mode;
  errno = ENOSYS;
  return -1;
}

int remove(const char *path) { return unlink(path); }

ssize_t readlink(const char *path, char *buf, size_t bufsiz) {
  (void)path;
  (void)buf;
  (void)bufsiz;
  errno = ENOSYS;
  return -1;
}

ssize_t getline(char **lineptr, size_t *n, FILE *stream) {
  (void)lineptr;
  (void)n;
  (void)stream;
  errno = ENOSYS;
  return -1;
}

int fscanf(FILE *f, const char *fmt, ...) {
  (void)f;
  (void)fmt;
  errno = ENOSYS;
  return 0;
}

int scanf(const char *fmt, ...) {
  (void)fmt;
  errno = ENOSYS;
  return 0;
}

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

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

/* qsort — 真正的快速排序（median-of-three + 小数组插入排序）
 * 原 implementation 是纯插入排序，O(n²) 最坏。 */
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
    /* 小数组用插入排序，避免递归开销 */
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
    /* median-of-three 选 pivot，放到 hi */
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
    /* 尾递归消除：对较短的一侧递归，较长侧循环 */
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

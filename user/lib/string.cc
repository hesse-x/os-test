/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <xos/errno.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations: functions in this file call each other (strdup uses
 * memcpy, strerror_r uses strerror [now from musl], bcopy uses memmove, bcmp
 * uses memcmp); C++ requires prior declarations */
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);
size_t strlen(const char *s);

size_t strlen(const char *s) {
  size_t n = 0;
  while (*s++)
    n++;
  return n;
}

int strcmp(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *(const unsigned char *)a - *(const unsigned char *)b;
}

int strncmp(const char *a, const char *b, size_t n) {
  while (n && *a && *a == *b) {
    a++;
    b++;
    n--;
  }
  if (!n)
    return 0;
  return *(const unsigned char *)a - *(const unsigned char *)b;
}

char *strcpy(char *dst, const char *src) {
  char *d = dst;
  while ((*d++ = *src++))
    ;
  return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
  char *d = dst;
  while (n && (*d++ = *src++))
    n--;
  while (n--)
    *d++ = '\0';
  return dst;
}

char *strcat(char *dst, const char *src) {
  char *d = dst + strlen(dst);
  while ((*d++ = *src++))
    ;
  return dst;
}

char *strncat(char *dst, const char *src, size_t n) {
  char *d = dst + strlen(dst);
  while (n && *src) {
    *d++ = *src++;
    n--;
  }
  *d = '\0';
  return dst;
}

char *strchr(const char *s, int c) {
  while (*s) {
    if (*s == (char)c)
      return (char *)s;
    s++;
  }
  if (c == '\0')
    return (char *)s;
  return nullptr;
}

char *strrchr(const char *s, int c) {
  const char *last = nullptr;
  for (;;) {
    if (*s == (char)c)
      last = s;
    if (!*s)
      break;
    s++;
  }
  return (char *)last;
}

char *strpbrk(const char *s, const char *accept) {
  while (*s) {
    const char *a = accept;
    while (*a) {
      if (*s == *a)
        return (char *)s;
      a++;
    }
    s++;
  }
  return nullptr;
}

size_t strspn(const char *s, const char *accept) {
  size_t n = 0;
  while (*s) {
    const char *a = accept;
    int found = 0;
    while (*a) {
      if (*s == *a) {
        found = 1;
        break;
      }
      a++;
    }
    if (!found)
      break;
    n++;
    s++;
  }
  return n;
}

size_t strcspn(const char *s, const char *reject) {
  size_t n = 0;
  while (*s) {
    const char *r = reject;
    int found = 0;
    while (*r) {
      if (*s == *r) {
        found = 1;
        break;
      }
      r++;
    }
    if (found)
      break;
    n++;
    s++;
  }
  return n;
}

void *memchr(const void *s, int c, size_t n) {
  const unsigned char *p = (const unsigned char *)s;
  while (n--) {
    if (*p == (unsigned char)c)
      return (void *)p;
    p++;
  }
  return nullptr;
}

void *memmem(const void *haystack, size_t haystacklen, const void *needle,
             size_t needlelen) {
  if (needlelen == 0)
    return (void *)haystack;
  if (haystacklen < needlelen)
    return nullptr;
  const unsigned char *h = (const unsigned char *)haystack;
  const unsigned char *n = (const unsigned char *)needle;
  for (size_t i = 0; i <= haystacklen - needlelen; i++) {
    size_t j = 0;
    for (; j < needlelen; j++)
      if (h[i + j] != n[j])
        break;
    if (j == needlelen)
      return (void *)(h + i);
  }
  return nullptr;
}

char *strdup(const char *s) {
  size_t n = strlen(s) + 1;
  char *p = (char *)malloc(n);
  if (!p) {
    errno = ENOMEM;
    return nullptr;
  }
  return (char *)memcpy(p, s, n);
}

char *strndup(const char *s, size_t n) {
  size_t len = 0;
  while (len < n && s[len])
    len++;
  char *p = (char *)malloc(len + 1);
  if (!p) {
    errno = ENOMEM;
    return nullptr;
  }
  memcpy(p, s, len);
  p[len] = '\0';
  return p;
}

int strerror_r(int errnum, char *buf, size_t buflen) {
  if (!buf || buflen == 0)
    return EINVAL;
  const char *msg = strerror(errnum);
  size_t len = 0;
  while (msg[len])
    len++;
  if (len >= buflen) {
    size_t copy = buflen - 1;
    memcpy(buf, msg, copy);
    buf[copy] = '\0';
    return ERANGE;
  }
  memcpy(buf, msg, len + 1);
  return 0;
}

/* BSD legacy aliases */
void bcopy(const void *src, void *dst, size_t n) { memmove(dst, src, n); }

int bcmp(const void *s1, const void *s2, size_t n) {
  return memcmp(s1, s2, n) != 0 ? 1 : 0;
}

int ffs(int i) { return __builtin_ffs(i); }

char *index(const char *s, int c) { return strchr(s, c); }
char *rindex(const char *s, int c) { return strrchr(s, c); }

void *memcpy(void *dst, const void *src, size_t n) {
  char *d = (char *)dst;
  const char *s = (const char *)src;
  while (n--)
    *d++ = *s++;
  return dst;
}

void *memset(void *s, int c, size_t n) {
  char *p = (char *)s;
  while (n--)
    *p++ = (char)c;
  return s;
}

void *memmove(void *dst, const void *src, size_t n) {
  char *d = (char *)dst;
  const char *s = (const char *)src;
  if (d < s) {
    size_t nq = n >> 3;
    uint64_t *dq = (uint64_t *)d;
    const uint64_t *sq = (const uint64_t *)s;
    while (nq--)
      *dq++ = *sq++;
    d = (char *)dq;
    s = (const char *)sq;
    n &= 7;
    while (n--)
      *d++ = *s++;
  } else {
    d += n;
    s += n;
    size_t tail = n & 7;
    while (tail--)
      *--d = *--s;
    size_t nq = n >> 3;
    while (nq--) {
      d -= 8;
      s -= 8;
      *(uint64_t *)d = *(const uint64_t *)s;
    }
  }
  return dst;
}

int memcmp(const void *s1, const void *s2, size_t n) {
  const unsigned char *a = (const unsigned char *)s1;
  const unsigned char *b = (const unsigned char *)s2;
  while (n--) {
    if (*a != *b)
      return (int)(*a - *b);
    a++;
    b++;
  }
  return 0;
}

char *strstr(const char *haystack, const char *needle) {
  if (!*needle)
    return (char *)haystack;
  while (*haystack) {
    const char *h = haystack;
    const char *n = needle;
    while (*h && *n && *h == *n) {
      h++;
      n++;
    }
    if (!*n)
      return (char *)haystack;
    haystack++;
  }
  return nullptr;
}

char *strtok_r(char *str, const char *delim, char **saveptr) {
  if (!str)
    str = *saveptr;
  if (!str)
    return nullptr;
  // Skip leading delimiters
  while (*str) {
    const char *d = delim;
    int is_delim = 0;
    while (*d) {
      if (*str == *d) {
        is_delim = 1;
        break;
      }
      d++;
    }
    if (!is_delim)
      break;
    str++;
  }
  if (!*str) {
    *saveptr = nullptr;
    return nullptr;
  }
  char *token = str;
  // Find end of token
  while (*str) {
    const char *d = delim;
    int is_delim = 0;
    while (*d) {
      if (*str == *d) {
        is_delim = 1;
        break;
      }
      d++;
    }
    if (is_delim) {
      *str = '\0';
      *saveptr = str + 1;
      return token;
    }
    str++;
  }
  *saveptr = nullptr;
  return token;
}

char *strtok(char *str, const char *delim) {
  static char *saveptr = nullptr;
  return strtok_r(str, delim, &saveptr);
}

/* strerror() is provided by musl's src/errno/strerror.c (merged into libc via
 * musl_pthread in user/CMakeLists.txt), not the hand-written switch that used
 * to live here — musl's __strerror.h table covers every errno with the
 * canonical Linux/glibc message. strerror_r below still calls it. */

void bzero(void *s, size_t n) {
  char *p = (char *)s;
  while (n--)
    *p++ = 0;
}

/* strcasecmp / strncasecmp — POSIX <strings.h> */
static unsigned char _toupper(unsigned char c) {
  if (c >= 'a' && c <= 'z')
    return (unsigned char)(c - 'a' + 'A');
  return c;
}

int strcasecmp(const char *s1, const char *s2) {
  while (*s1 && _toupper((unsigned char)*s1) == _toupper((unsigned char)*s2)) {
    s1++;
    s2++;
  }
  return (int)_toupper((unsigned char)*s1) - (int)_toupper((unsigned char)*s2);
}

int strncasecmp(const char *s1, const char *s2, size_t n) {
  while (n && *s1 &&
         _toupper((unsigned char)*s1) == _toupper((unsigned char)*s2)) {
    s1++;
    s2++;
    n--;
  }
  if (!n)
    return 0;
  return (int)_toupper((unsigned char)*s1) - (int)_toupper((unsigned char)*s2);
}

char *basename(char *path) {
  char *p = strrchr(path, '/');
  return p ? p + 1 : path;
}

#ifdef __cplusplus
}
#endif

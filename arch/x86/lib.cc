#include <stdint.h>
#include "arch/x86/lib.h"

void *memcpy(void *dst, const void *src, size_t n) {
  unsigned char *d = (unsigned char *)dst;
  const unsigned char *s = (const unsigned char *)src;
  while (n--)
    *d++ = *s++;
  return dst;
}
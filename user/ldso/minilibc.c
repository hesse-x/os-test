/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// ld.so's built-in minimal libc (does not link libc.a)
// ld.md §7.1 deviation: ld.so does not link libc.a, brings its own minimal libc

#include <stddef.h>
#include <stdint.h>
#include <xos/mman.h>

void *memcpy(void *dst, const void *src, unsigned long n) {
  char *d = (char *)dst;
  const char *s = (const char *)src;
  while (n--)
    *d++ = *s++;
  return dst;
}

void *memset(void *dst, int c, unsigned long n) {
  char *d = (char *)dst;
  while (n--)
    *d++ = (char)c;
  return dst;
}

int strcmp(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

unsigned long strlen(const char *s) {
  unsigned long n = 0;
  while (s[n])
    n++;
  return n;
}

// exported by load_so.c (hidden), reuse the same mmap wrapper, avoiding
// duplicate inline asm
__attribute__((visibility("hidden"))) void *dl_sys_mmap(void *addr, size_t size,
                                                        int prot, int flags,
                                                        int fd,
                                                        uint64_t offset);

// simple malloc: round size up to 4KB pages, dl_sys_mmap anonymous mapping;
// no free (ld.so allocations happen during load, no reclamation needed)
void *malloc(unsigned long size) {
  unsigned long page = 4096;
  size = (size + page - 1) & ~(page - 1);
  if (size == 0)
    size = page;
  return dl_sys_mmap(NULL, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

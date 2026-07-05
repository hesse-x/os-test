/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// ld.so 自带最小 libc（不链 libc.a）
// ld.md §7.1 偏离：ld.so 不链 libc.a，自带最小 libc

#include "xos/mman.h"
#include <stddef.h>
#include <stdint.h>

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

// load_so.c 导出（hidden），复用同一 mmap 封装，避免重复内联汇编
__attribute__((visibility("hidden"))) void *dl_sys_mmap(void *addr, size_t size,
                                                        int prot, int flags,
                                                        int fd,
                                                        uint64_t offset);

// 简单 malloc：按 size 向上取整到 4KB 页，dl_sys_mmap 匿名映射；
// 不 free（ld.so 加载期分配，无需回收）
void *malloc(unsigned long size) {
  unsigned long page = 4096;
  size = (size + page - 1) & ~(page - 1);
  if (size == 0)
    size = page;
  return dl_sys_mmap(NULL, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

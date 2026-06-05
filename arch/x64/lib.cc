#include <stdint.h>
#include "arch/x64/lib.h"

void *memcpy(void *dst, const void *src, size_t n) {
  unsigned char *d = (unsigned char *)dst;
  const unsigned char *s = (const unsigned char *)src;
  while (n--)
    *d++ = *s++;
  return dst;
}

// 早期串口输出，不需要初始化，直接 outb 到 COM1
void serial_early_out(char c) {
  __asm__ volatile("outb %0, $0x3F8" :: "a"(c));
}

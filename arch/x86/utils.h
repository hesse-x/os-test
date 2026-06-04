#ifndef ARCH_X86_UTILS_H
#define ARCH_X86_UTILS_H

#include <stdint.h>

// ===================== I/O port helpers =====================
static inline void outb(uint16_t port, uint8_t val) {
  __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
  uint8_t ret;
  __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

// ===================== Constants =====================
#define KERNEL_CS 0x08

#define L16(x) ((uint16_t)((x) & 0xFFFF))
#define H16(x) ((uint16_t)(((x) >> 16) & 0xFFFF))

#endif // ARCH_X86_UTILS_H

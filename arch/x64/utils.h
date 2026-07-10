/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ARCH_X64_UTILS_H
#define ARCH_X64_UTILS_H

#include "kernel/xcore/sparse.h"
#include <stddef.h>
#include <stdint.h>
#include <xos/syscall_nums.h> // recv_msg / pci_dev_info / SYS_* (UAPI)

// ===================== I/O port helpers =====================
static inline void outb(uint16_t port, uint8_t val) {
  __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
  uint8_t ret;
  __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

// ===================== MSR helpers =====================
static inline void wrmsr(uint32_t msr, uint64_t val) {
  uint32_t lo = val & 0xFFFFFFFF;
  uint32_t hi = (val >> 32) & 0xFFFFFFFF;
  __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static inline uint64_t rdmsr(uint32_t msr) {
  uint32_t lo, hi;
  __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
  return ((uint64_t)hi << 32) | lo;
}

// ===================== Memory helpers =====================
__attribute__((no_sanitize("kernel-address"))) static inline void *
__memcpy(void *dst, const void *src, size_t n) {
  size_t nq = n >> 3;
  uint64_t *dq = (uint64_t *)dst;
  const uint64_t *sq = (const uint64_t *)src;
  while (nq--)
    *dq++ = *sq++;
  unsigned char *db = (unsigned char *)dq;
  const unsigned char *sb = (const unsigned char *)sq;
  n &= 7;
  while (n--)
    *db++ = *sb++;
  return dst;
}

__attribute__((no_sanitize("kernel-address"))) static inline void *
__memset(void *dst, int val, size_t n) {
  size_t nq = n >> 3;
  uint64_t *dq = (uint64_t *)dst;
  uint64_t v8 = (uint8_t)val;
  v8 |= v8 << 8;
  v8 |= v8 << 16;
  v8 |= v8 << 32;
  while (nq--)
    *dq++ = v8;
  unsigned char *db = (unsigned char *)dq;
  n &= 7;
  while (n--)
    *db++ = (uint8_t)val;
  return dst;
}

__attribute__((no_sanitize("kernel-address"))) static inline void *
__memmove(void *dst, const void *src, size_t n) {
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

__attribute__((no_sanitize("kernel-address"))) static inline int
__memcmp(const void *s1, const void *s2, size_t n) {
  const unsigned char *a = (const unsigned char *)s1;
  const unsigned char *b = (const unsigned char *)s2;
  size_t i = 0;
  // 8-byte batch compare; on mismatch, backtrack byte-by-byte to find first
  // difference
  for (; i + 8 <= n; i += 8) {
    uint64_t va = *(const uint64_t *)(a + i);
    uint64_t vb = *(const uint64_t *)(b + i);
    if (va != vb) {
      for (size_t j = 0; j < 8; j++) {
        if (a[i + j] != b[i + j])
          return (int)a[i + j] - (int)b[i + j];
      }
    }
  }
  // Remaining bytes (< 8)
  for (; i < n; i++) {
    if (a[i] != b[i])
      return (int)a[i] - (int)b[i];
  }
  return 0;
}

// ===================== SWAR helpers =====================
// SIMD Within A Register: detect NUL bytes or specific byte values in 64-bit
// words. Used by __strcmp/__strncmp/__strlen/__strchr for 8× throughput over
// byte-by-byte scanning.

// Detect if any byte in v is zero. Returns a bitmask where bit 7 of each
// byte-position is set if that byte was zero.
static inline uint64_t __swar_has_zero(uint64_t v) {
  return (v - 0x0101010101010101ULL) & ~v & 0x8080808080808080ULL;
}

// Detect if any byte in v equals c.
static inline uint64_t __swar_has_byte(uint64_t v, uint8_t c) {
  return __swar_has_zero(v ^ ((uint64_t)c * 0x0101010101010101ULL));
}

// Given a SWAR bitmask, return the byte index (0–7) of the first flagged byte.
static inline unsigned __swar_first_idx(uint64_t mask) {
  return (unsigned)__builtin_ctzll(mask) >> 3;
}

// ===================== String helpers =====================
__attribute__((no_sanitize("kernel-address"))) static inline int
__strcmp(const char *s1, const char *s2) {
  const unsigned char *a = (const unsigned char *)s1;
  const unsigned char *b = (const unsigned char *)s2;
  for (;;) {
    uint64_t wa = *(const uint64_t *)a;
    uint64_t wb = *(const uint64_t *)b;
    uint64_t nz = __swar_has_zero(wa) | __swar_has_zero(wb);
    if (nz) {
      // NUL in this chunk — resolve byte-by-byte
      for (unsigned j = 0; j < 8; j++) {
        if (a[j] != b[j] || a[j] == 0)
          return (int)a[j] - (int)b[j];
      }
      return 0;
    }
    if (wa != wb) {
      for (unsigned j = 0; j < 8; j++) {
        if (a[j] != b[j])
          return (int)a[j] - (int)b[j];
      }
    }
    a += 8;
    b += 8;
  }
}

__attribute__((no_sanitize("kernel-address"))) static inline int
__strncmp(const char *s1, const char *s2, size_t n) {
  if (!n)
    return 0;
  const unsigned char *a = (const unsigned char *)s1;
  const unsigned char *b = (const unsigned char *)s2;
  size_t i = 0;
  // 8-byte batch; stop early if either word contains NUL
  for (; i + 8 <= n; i += 8) {
    uint64_t wa = *(const uint64_t *)(a + i);
    uint64_t wb = *(const uint64_t *)(b + i);
    uint64_t nz = __swar_has_zero(wa) | __swar_has_zero(wb);
    if (nz) {
      for (unsigned j = 0; j < 8 && i + j < n; j++) {
        if (a[i + j] != b[i + j] || a[i + j] == 0)
          return (int)a[i + j] - (int)b[i + j];
      }
      return 0;
    }
    if (wa != wb) {
      for (unsigned j = 0; j < 8 && i + j < n; j++) {
        if (a[i + j] != b[i + j])
          return (int)a[i + j] - (int)b[i + j];
      }
    }
  }
  // Remaining bytes (< 8)
  for (; i < n; i++) {
    if (a[i] != b[i] || a[i] == 0)
      return (int)a[i] - (int)b[i];
  }
  return 0;
}

__attribute__((no_sanitize("kernel-address"))) static inline size_t
__strlen(const char *s) {
  const char *p = s;
  for (;;) {
    uint64_t w = *(const uint64_t *)p;
    uint64_t z = __swar_has_zero(w);
    if (z)
      return (size_t)(p - s) + __swar_first_idx(z);
    p += 8;
  }
}

__attribute__((no_sanitize("kernel-address"))) static inline size_t
__strnlen(const char *s, size_t maxlen) {
  const char *p = s;
  size_t rem = maxlen;
  while (rem >= 8) {
    uint64_t w = *(const uint64_t *)p;
    uint64_t z = __swar_has_zero(w);
    if (z) {
      size_t pos = (size_t)(p - s) + __swar_first_idx(z);
      return pos < maxlen ? pos : maxlen;
    }
    p += 8;
    rem -= 8;
  }
  for (size_t i = 0; i < rem; i++) {
    if (p[i] == '\0')
      return (size_t)(p - s) + i;
  }
  return maxlen;
}

__attribute__((no_sanitize("kernel-address"))) static inline void
__strncpy(char *dst, const char *src, size_t n) {
  size_t slen = __strnlen(src, n);
  __memcpy(dst, src, slen);
  __memset(dst + slen, 0, n - slen);
}

__attribute__((no_sanitize("kernel-address"))) static inline char *
__strchr(const char *s, int c) {
  if (c == 0)
    return (char *)(s + __strlen(s));
  const char *p = s;
  for (;;) {
    uint64_t w = *(const uint64_t *)p;
    uint64_t z = __swar_has_zero(w);
    if (z) {
      // String ends in this word — check for target byte before NUL
      unsigned zi = __swar_first_idx(z);
      uint64_t m = __swar_has_byte(w, (uint8_t)c);
      if (m) {
        unsigned mi = __swar_first_idx(m);
        if (mi < zi)
          return (char *)(p + mi);
      }
      return NULL;
    }
    uint64_t m = __swar_has_byte(w, (uint8_t)c);
    if (m)
      return (char *)(p + __swar_first_idx(m));
    p += 8;
  }
}

// ===================== Interrupt control =====================
static inline void cli() { __asm__ volatile("cli"); }

static inline void sti() { __asm__ volatile("sti"); }

static inline void halt() { __asm__ volatile("cli; hlt"); }

// ===================== MMIO helpers =====================
__attribute__((no_sanitize("kernel-address"))) static inline uint16_t
mmio_read16(const volatile void __iomem *addr) {
  return *(volatile const uint16_t __force *)addr;
}

__attribute__((no_sanitize("kernel-address"))) static inline void
mmio_write16(volatile void __iomem *addr, uint16_t val) {
  *(volatile uint16_t __force *)addr = val;
}

__attribute__((no_sanitize("kernel-address"))) static inline uint32_t
readl(const void __iomem *addr) {
  return *(volatile const uint32_t __force *)addr;
}

__attribute__((no_sanitize("kernel-address"))) static inline void
writel(void __iomem *addr, uint32_t val) {
  *(volatile uint32_t __force *)addr = val;
}

// ===================== System register helpers =====================
static inline void load_cr3(uint64_t addr) {
  __asm__ volatile("movq %0, %%cr3" ::"r"(addr) : "memory");
}

static inline uint64_t read_cr4() {
  uint64_t val;
  __asm__ volatile("movq %%cr4, %0" : "=r"(val));
  return val;
}

static inline uint64_t read_cr0() {
  uint64_t val;
  __asm__ volatile("movq %%cr0, %0" : "=r"(val));
  return val;
}

static inline void write_cr4(uint64_t val) {
  __asm__ volatile("movq %0, %%cr4" ::"r"(val) : "memory");
}

static inline void lgdt(const void *ptr) {
  __asm__ volatile("lgdt %0" : : "m"(*((const uint64_t *)ptr)));
}

static inline void lidt(const void *ptr) {
  __asm__ volatile("lidt %0" : : "m"(*((const uint64_t *)ptr)));
}

static inline void ltr(uint16_t sel) { __asm__ volatile("ltr %w0" ::"r"(sel)); }

static inline void invlpg(uint64_t vaddr) {
  __asm__ volatile("invlpg (%0)" ::"r"(vaddr) : "memory");
}

static inline uint64_t read_cr2() {
  uint64_t val;
  __asm__ volatile("movq %%cr2, %0" : "=r"(val));
  return val;
}

// ===================== TSC helpers =====================
static inline uint64_t rdtsc64() {
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}

// ===================== Constants =====================
#define KERNEL_CS 0x08

#define L16(x) ((uint16_t)((x) & 0xFFFF))
#define H16(x) ((uint16_t)(((x) >> 16) & 0xFFFF))
#define L32(x) ((uint32_t)((x) & 0xFFFFFFFF))
#define H32(x) ((uint32_t)(((x) >> 32) & 0xFFFFFFFF))

// ===================== Syscall =====================
// SYSCALL/SYSRET calling convention (Linux-style):
//   RAX = syscall number, RDI/RSI/RDX/R10/R8/R9 = args
//   RAX = return value
//   RCX = saved RIP, R11 = saved RFLAGS (clobbered by SYSCALL)
// The __syscallN inline-assembly wrappers live in xos/syscall_asm.h
// (UAPI, self-contained); semantic wrappers are in user/include/syscall.h.
#include <xos/syscall_asm.h>

#include <xos/shm.h>

#endif // ARCH_X64_UTILS_H

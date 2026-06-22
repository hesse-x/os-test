#ifndef ARCH_X64_UTILS_H
#define ARCH_X64_UTILS_H

#include <stddef.h>
#include <stdint.h>
#include "kernel/sparse.h"

// ===================== I/O port helpers =====================
static inline void outb(uint16_t port, uint8_t val) {
  __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
  uint8_t ret;
  __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

static inline uint16_t inw(uint16_t port) {
  uint16_t ret;
  __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
  __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
  uint32_t ret;
  __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

static inline void outl(uint16_t port, uint32_t val) {
  __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
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
static inline void *__memcpy(void *dst, const void *src, size_t n) {
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

static inline void *__memset(void *dst, int val, size_t n) {
  size_t nq = n >> 3;
  uint64_t *dq = (uint64_t *)dst;
  uint64_t v8 = (uint8_t)val;
  v8 |= v8 << 8; v8 |= v8 << 16; v8 |= v8 << 32;
  while (nq--)
    *dq++ = v8;
  unsigned char *db = (unsigned char *)dq;
  n &= 7;
  while (n--)
    *db++ = (uint8_t)val;
  return dst;
}

// ===================== Early serial output =====================
#ifdef NDEBUG
static inline void serial_early_out(char c) { (void)c; }
#else
static inline void serial_early_out(char c) { outb(0x3F8, c); }
#endif

// ===================== Interrupt control =====================
static inline void cli() {
  __asm__ volatile("cli");
}

static inline void sti() {
  __asm__ volatile("sti");
}

static inline void halt() {
  __asm__ volatile("cli; hlt");
}

// ===================== Interrupt state save/restore =====================
static inline uint64_t local_irq_save() {
  uint64_t flags;
  __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags));
  return flags;
}

static inline void local_irq_restore(uint64_t flags) {
  __asm__ volatile("pushq %0; popfq" :: "r"(flags));
}

// ===================== RAII interrupt guard (C cleanup macro) =====================
static inline void irq_guard_cleanup(uint64_t *flags) {
  local_irq_restore(*flags);
}

#define DEFINE_IRQGUARD(name) \
  uint64_t name __attribute__((cleanup(irq_guard_cleanup))) = local_irq_save()

// ===================== MMIO helpers =====================
static inline uint32_t readl(const void __iomem *addr) {
  return *(volatile const uint32_t __force *)addr;
}

static inline void writel(void __iomem *addr, uint32_t val) {
  *(volatile uint32_t __force *)addr = val;
}

// ===================== System register helpers =====================
static inline void load_cr3(uint64_t addr) {
  __asm__ volatile("movq %0, %%cr3" :: "r"(addr) : "memory");
}

static inline uint64_t read_cr4() {
  uint64_t val;
  __asm__ volatile("movq %%cr4, %0" : "=r"(val));
  return val;
}

static inline void write_cr4(uint64_t val) {
  __asm__ volatile("movq %0, %%cr4" :: "r"(val) : "memory");
}

static inline void lgdt(const void *ptr) {
  __asm__ volatile("lgdt %0" : : "m"(*((const uint64_t *)ptr)));
}

static inline void lidt(const void *ptr) {
  __asm__ volatile("lidt %0" : : "m"(*((const uint64_t *)ptr)));
}

static inline void ltr(uint16_t sel) {
  __asm__ volatile("ltr %w0" :: "r"(sel));
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
// Semantic wrappers are in common/syscall.h
static inline int64_t __syscall0(int64_t num) {
  int64_t ret;
  __asm__ volatile(
      "syscall"
      : "=a"(ret)
      : "a"(num)
      : "rcx", "r11", "memory");
  return ret;
}

static inline int64_t __syscall1(int64_t num, int64_t arg1) {
  int64_t ret;
  __asm__ volatile(
      "syscall"
      : "=a"(ret)
      : "a"(num), "D"(arg1)
      : "rcx", "r11", "memory");
  return ret;
}

static inline int64_t __syscall2(int64_t num, int64_t arg1, int64_t arg2) {
  int64_t ret;
  __asm__ volatile(
      "syscall"
      : "=a"(ret)
      : "a"(num), "D"(arg1), "S"(arg2)
      : "rcx", "r11", "memory");
  return ret;
}

static inline int64_t __syscall3(int64_t num, int64_t arg1, int64_t arg2, int64_t arg3) {
  int64_t ret;
  __asm__ volatile(
      "syscall"
      : "=a"(ret)
      : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3)
      : "rcx", "r11", "memory");
  return ret;
}

static inline int64_t __syscall4(int64_t num, int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4) {
  int64_t ret;
  register int64_t a4 __asm__("r10") = arg4;
  __asm__ volatile(
      "syscall"
      : "=a"(ret)
      : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3), "r"(a4)
      : "rcx", "r11", "memory");
  return ret;
}

static inline int64_t __syscall5(int64_t num, int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4, int64_t arg5) {
  int64_t ret;
  register int64_t a4 __asm__("r10") = arg4;
  register int64_t a5 __asm__("r8") = arg5;
  __asm__ volatile(
      "syscall"
      : "=a"(ret)
      : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3), "r"(a4), "r"(a5)
      : "rcx", "r11", "memory");
  return ret;
}

static inline int64_t __syscall6(int64_t num, int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4, int64_t arg5, int64_t arg6) {
  int64_t ret;
  register int64_t a4 __asm__("r10") = arg4;
  register int64_t a5 __asm__("r8") = arg5;
  register int64_t a6 __asm__("r9") = arg6;
  __asm__ volatile(
      "syscall"
      : "=a"(ret)
      : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3), "r"(a4), "r"(a5), "r"(a6)
      : "rcx", "r11", "memory");
  return ret;
}

#include "common/syscall.h"
#include "common/shm.h"

#endif // ARCH_X64_UTILS_H

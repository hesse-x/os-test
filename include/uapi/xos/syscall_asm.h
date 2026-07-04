#ifndef COMMON_SYSCALL_ASM_H
#define COMMON_SYSCALL_ASM_H

/*
 * Pure SYSCALL/SYSRET inline-assembly wrappers — UAPI, self-contained.
 *
 * Shared by:
 *   - kernel  (arch/x64/utils.h includes this, though the kernel implements
 *              syscalls rather than issuing them)
 *   - userspace (user/include/syscall.h, the semantic sys_* wrappers)
 *
 * Keeping these out of arch/x64/utils.h lets the userspace wrappers depend
 * only on this UAPI header instead of pulling in kernel-internal helpers
 * (IrqGuard, outb/inb, MSR access, ...) from utils.h.
 *
 * Calling convention (Linux-style):
 *   RAX = syscall number, RDI/RSI/RDX/R10/R8/R9 = args
 *   RAX = return value
 *   RCX = saved RIP, R11 = saved RFLAGS (clobbered by SYSCALL)
 * Semantic wrappers live in user/include/syscall.h.
 */

#include <stdint.h>

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

#endif /* COMMON_SYSCALL_ASM_H */

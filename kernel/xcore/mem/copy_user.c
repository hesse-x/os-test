/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// copy_from_user / copy_to_user / strncpy_from_user — out-of-line to prevent
// the compiler from inlining __memcpy and mis-optimizing source writes via
// strict-aliasing violations.
//
// These are the single canonical implementation, built for both the normal and
// the SANITIZER (KASAN) configurations. User pointers are never in the KASAN
// shadow range, so the body bypasses KASAN via no_sanitize("kernel-address").
// Faulting loads/stores are annotated with _ASM_EXTABLE so a kernel-mode #PF
// or #GP on an unmapped/non-canonical user pointer unwinds to a fixup that
// returns -EFAULT instead of panicking. access_ok() rejects kernel-space
// pointers up front (failure mode #2: silent kernel-memory leak with no fault).

#include <stdbool.h>
#include <stddef.h>

#include "kernel/user_check.h"
#include "kernel/xcore/mem/extable.h"
#include "kernel/xcore/mem/kasan.h"
#include "kernel/xcore/sparse.h"

#include <xos/errno.h>

// access_ok: reject kernel-space pointers and overflows (failure mode #2:
// user passes a pointer >= KERNEL_VMA_BOUNDARY; higher-half kernel mapping is
// present+RW in the process space, __memcpy "succeeds" with no fault, silently
// copying kernel memory). Short-circuits size==0 to success.
static inline bool access_ok(const void __user *ptr, size_t size) {
  if (size == 0)
    return true;
  return validate_user_buf(ptr, size);
}

// copy_from_user: 0 = success, nonzero = -EFAULT.
// Body is inline asm rep movsb so the faulting load is a single instruction
// with a known label; _ASM_EXTABLE marks it, and fixup_exception() rewrites
// tf->rip to the fixup label on a kernel-mode #PF/#GP.
__attribute__((no_sanitize("kernel-address"))) size_t
copy_from_user(void *dst, const void __user *src, size_t size) {
  if (!access_ok(src, size))
    return (size_t)-EFAULT;

  size_t ret = 0;
  __asm__ volatile("1: rep movsb\n"
                   "   xorq %0, %0\n" // success: ret = 0
                   "   jmp 3f\n"
                   ".section .text\n"
                   "2: movq $-14, %0\n" // fixup: ret = -EFAULT (EFAULT=14)
                   "   jmp 3f\n"
                   ".previous\n"
                   "3:\n" _ASM_EXTABLE("1b", "2b")
                   : "=&a"(ret), "+D"(dst), "+S"(src), "+c"(size)
                   :
                   : "memory");
  return ret;
}

// copy_to_user: same convention as copy_from_user.
__attribute__((no_sanitize("kernel-address"))) size_t
copy_to_user(void __user *dst, const void *src, size_t size) {
  if (!access_ok(dst, size))
    return (size_t)-EFAULT;

  size_t ret = 0;
  __asm__ volatile("1: rep movsb\n"
                   "   xorq %0, %0\n" // success: ret = 0
                   "   jmp 3f\n"
                   ".section .text\n"
                   "2: movq $-14, %0\n" // fixup: ret = -EFAULT (EFAULT=14)
                   "   jmp 3f\n"
                   ".previous\n"
                   "3:\n" _ASM_EXTABLE("1b", "2b")
                   : "=&a"(ret), "+D"(dst), "+S"(src), "+c"(size)
                   :
                   : "memory");
  return ret;
}

// strncpy_from_user: returns string length (excl. '\0') on success,
// -EFAULT on fault. Each byte load is its own labeled instruction with its
// own extable entry, so a fault on any byte unwinds to set %rax=-EFAULT.
__attribute__((no_sanitize("kernel-address"))) long
strncpy_from_user(char *dst, const char __user *src, long maxlen) {
  if (!src || maxlen <= 0)
    return -EFAULT;
  if (!access_ok(src, (size_t)maxlen))
    return -EFAULT;

  long len = 0;
  long ret = 0;
  char tmp;
  __asm__ volatile("1: movb (%4,%2), %1\n" // load src[len] -> tmp
                   "   movb %1, (%3,%2)\n" // store tmp -> dst[len]
                   "   testb %1, %1\n"     // NUL?
                   "   jz 4f\n"            // yes -> success exit
                   "   incq %2\n"          // len++
                   "   cmpq %5, %2\n"      // len < maxlen?
                   "   jb 1b\n"            // yes -> next byte
                   "4: xorq %0, %0\n"      // success: ret = 0
                   "   jmp 3f\n"
                   ".section .text\n"
                   "2: movq $-14, %0\n" // fixup: ret = -EFAULT (EFAULT=14)
                   "   jmp 3f\n"
                   ".previous\n"
                   "3:\n" _ASM_EXTABLE("1b", "2b")
                   : "=&a"(ret), "=&r"(tmp), "+r"(len)
                   : "r"(dst), "r"(src), "r"(maxlen)
                   : "memory");
  if (ret == 0)
    return len; // success path: ret=0, return length
  return -EFAULT;
}

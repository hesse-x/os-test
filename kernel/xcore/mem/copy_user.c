// copy_from_user / copy_to_user / strncpy_from_user — out-of-line to prevent
// the compiler from inlining __memcpy and mis-optimizing source writes via
// strict-aliasing violations.
//
// When SANITIZER is defined, these live in kasan.c (with shadow checks).
// This file provides the non-sanitizer versions.

#ifndef SANITIZER

#include <stddef.h>
#include "common/errno.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/mem/kasan.h"

__attribute__((no_sanitize("kernel-address")))
size_t copy_from_user(void *dst, const void __user *src, size_t size) {
    __memcpy(dst, (const void __force *)src, size);
    return 0;
}

__attribute__((no_sanitize("kernel-address")))
size_t copy_to_user(void __user *dst, const void *src, size_t size) {
    __memcpy((void __force *)dst, src, size);
    return 0;
}

// strncpy_from_user — copy a string from user space byte-by-byte.
// Stops at '\0' or maxlen, never crosses page boundaries for short strings
// near page edges (unlike copy_from_user which reads a fixed block).
// Returns: length of string (excluding '\0'), or -EFAULT if src is NULL.
long strncpy_from_user(char *dst, const char __user *src, long maxlen) {
    if (!src || maxlen <= 0) return -EFAULT;
    long len = 0;
    while (len < maxlen) {
        char c = *(volatile const char __force *)(src + len);
        dst[len] = c;
        if (c == '\0') return len;
        len++;
    }
    dst[maxlen - 1] = '\0';
    return len;
}

#endif /* !SANITIZER */

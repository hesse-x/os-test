// copy_from_user / copy_to_user — out-of-line to prevent the compiler
// from inlining __memcpy and mis-optimizing source writes via
// strict-aliasing violations.
//
// When SANITIZER is defined, these live in kasan.c (with shadow checks).
// This file provides the non-sanitizer versions.

#ifndef SANITIZER

#include <stddef.h>
#include "kernel/sparse.h"
#include "kernel/mem/kasan.h"

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

#endif /* !SANITIZER */

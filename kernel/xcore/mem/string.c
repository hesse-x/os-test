/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
//
// Freestanding C runtime: the standard <string.h> mem* functions.
//
// Why this exists: the kernel links with a bare `ld` (no libgcc/libc), and the
// C standard requires freestanding environments to provide mem* themselves
// (C99 7.21.1). gcc happens to inline struct zero-initializers (`*p = (T){0}`)
// and never emits a memset call, so the kernel historically linked without
// these definitions. clang instead lowers large aggregate zeroing to a library
// `memset`/`memcpy` call even under -ffreestanding -fno-builtin, leaving the
// symbol undefined at link time. Providing the canonical names here makes the
// build work under both compilers without changing any existing call site.
//
// The bodies delegate to the existing __memcpy/__memset/__memmove/__memcmp
// static-inline helpers in arch/x64/utils.h, which are already annotated
// __attribute__((no_sanitize("kernel-address"))) so KASAN builds don't poison
// on the raw byte loops. Wrapping rather than duplicating keeps a single
// verified implementation of each.

#include <stddef.h>

#include "arch/x64/utils.h"

// Forward declarations so sparse doesn't warn "symbol was not declared, should
// it be static?" on the definitions below. These are deliberately external
// (not static): clang lowers large aggregate zeroing/copying to library calls
// to these exact names, and the linker must resolve them. No kernel call site
// uses the canonical names directly — they exist only for the compiler.
void *memset(void *dst, int val, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);

void *memset(void *dst, int val, size_t n) { return __memset(dst, val, n); }

void *memcpy(void *dst, const void *src, size_t n) {
  return __memcpy(dst, src, n);
}

void *memmove(void *dst, const void *src, size_t n) {
  return __memmove(dst, src, n);
}

int memcmp(const void *s1, const void *s2, size_t n) {
  return __memcmp(s1, s2, n);
}

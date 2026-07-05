/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_MEM_KASAN_H
#define KERNEL_MEM_KASAN_H

#include "arch/x64/utils.h" // __memcpy (static inline, must be visible for copy_from_user/copy_to_user)
#include "kernel/xcore/sparse.h" // __user, __force
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef SANITIZER

// Shadow offset: addr >> 3 + offset = shadow address
#define KASAN_SHADOW_OFFSET 0xDFFFFC0000000000ULL
#define KASAN_SHADOW_SCALE 3 // 1 shadow byte = 8 memory bytes

// Shadow byte values
#define KASAN_SHADOW_VALID 0x00
#define KASAN_SHADOW_REDZONE 0xFD
#define KASAN_SHADOW_FREED 0xFE // use-after-free

// Convert address <-> shadow
#define KASAN_MEM_TO_SHADOW(addr)                                              \
  ((uint8_t *)(((uint64_t)(addr) >> KASAN_SHADOW_SCALE) + KASAN_SHADOW_OFFSET))
#define KASAN_SHADOW_TO_MEM(shadow)                                            \
  ((void *)(((uint64_t)(shadow)-KASAN_SHADOW_OFFSET) << KASAN_SHADOW_SCALE))

// Shadow memory range: covers 0xFFFFFFFF80000000 ~ 0xFFFFFFFFFFFFFFFF
// = 2GB virtual, 256MB shadow
#define KASAN_SHADOW_START KASAN_MEM_TO_SHADOW(0xFFFFFFFF80000000ULL)
#define KASAN_SHADOW_END KASAN_MEM_TO_SHADOW(0xFFFFFFFFFFFFFFFFULL)
#define KASAN_SHADOW_SIZE                                                      \
  ((uint64_t)KASAN_SHADOW_END - (uint64_t)KASAN_SHADOW_START)

// Initialization (called between kernel_init_finish and slab_init)
void kasan_init(void);

// Shadow manipulation
void kasan_unpoison_shadow(const void *addr, size_t size);
void kasan_poison_shadow(const void *addr, size_t size, uint8_t value);

// Access checking (called by compiler-injected code)
void kasan_check_read(const void *addr, size_t size);
void kasan_check_write(const void *addr, size_t size);

// Returns true after kasan_init
bool kasan_shadow_exists(void);

// copy_from_user / copy_to_user (bypass shadow check for user addresses)
size_t copy_from_user(void *dst, const void __user *src, size_t size);
size_t copy_to_user(void __user *dst, const void *src, size_t size);
long strncpy_from_user(char *dst, const char __user *src, long maxlen);

// Global variable poisoning (called from kasan_init)
void kasan_poison_globals(void);

// Slab/BFC hooks
void kasan_slab_alloc(const void *object, size_t size);
void kasan_slab_free(const void *object, size_t size);
void kasan_bfc_alloc(const void *addr, size_t size);
void kasan_bfc_free(const void *addr, size_t size);

#else /* !SANITIZER */

// Non-sanitizer build: all functions are no-ops
static inline void kasan_init(void) {}
static inline void kasan_unpoison_shadow(const void *a, size_t s) {
  (void)a;
  (void)s;
}
static inline void kasan_poison_shadow(const void *a, size_t s, uint8_t v) {
  (void)a;
  (void)s;
  (void)v;
}
static inline void kasan_check_read(const void *a, size_t s) {
  (void)a;
  (void)s;
}
static inline void kasan_check_write(const void *a, size_t s) {
  (void)a;
  (void)s;
}
static inline bool kasan_shadow_exists(void) { return false; }

// copy_from_user / copy_to_user are out-of-line (defined in kasan.c)
// to prevent the compiler from inlining __memcpy and mis-optimizing
// source writes via strict-aliasing violations.
size_t copy_from_user(void *d, const void __user *s, size_t n);
size_t copy_to_user(void __user *d, const void *s, size_t n);
long strncpy_from_user(char *dst, const char __user *src, long maxlen);

static inline void kasan_slab_alloc(const void *a, size_t s) {
  (void)a;
  (void)s;
}
static inline void kasan_slab_free(const void *a, size_t s) {
  (void)a;
  (void)s;
}
static inline void kasan_bfc_alloc(const void *a, size_t s) {
  (void)a;
  (void)s;
}
static inline void kasan_bfc_free(const void *a, size_t s) {
  (void)a;
  (void)s;
}

#endif /* SANITIZER */

#endif /* KERNEL_MEM_KASAN_H */

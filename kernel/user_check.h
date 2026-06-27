#ifndef KERNEL_USER_CHECK_H
#define KERNEL_USER_CHECK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "common/macro.h"  // KERNEL_VMA_BOUNDARY

// Validate that a user-space buffer [buf, buf+len) does not cross into kernel space.
// Returns true if valid, false if invalid (NULL, overflow, or >= KERNEL_VMA_BOUNDARY).
static inline bool validate_user_buf(const void *buf, size_t len) {
    uint64_t start = (uint64_t)buf;
    uint64_t end = start + len;
    if (!start || end < start || start >= KERNEL_VMA_BOUNDARY || end > KERNEL_VMA_BOUNDARY)
        return false;
    return true;
}

// Single-pointer check (no length)
static inline bool validate_user_ptr(const void *ptr) {
    uint64_t v = (uint64_t)ptr;
    return v && v < KERNEL_VMA_BOUNDARY;
}

#endif /* KERNEL_USER_CHECK_H */

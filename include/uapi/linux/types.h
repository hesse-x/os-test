/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * Shim header: maps <linux/types.h> (used by upstream drm.h) to our
 * freestanding <stdint.h> types. Placed under include/uapi/linux/ so
 * -Iinclude/uapi resolves <linux/types.h> to this file.
 */

#ifndef _LINUX_TYPES_H
#define _LINUX_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef uint8_t __u8;
typedef int8_t __s8;
typedef uint16_t __u16;
typedef int16_t __s16;
typedef uint32_t __u32;
typedef int32_t __s32;
typedef uint64_t __u64;
typedef int64_t __s64;

typedef uint64_t __u64_t;
typedef int64_t __s64_t;
typedef uint32_t __u32_t;
typedef int32_t __s32_t;
typedef uint16_t __u16_t;
typedef int16_t __s16_t;

typedef size_t __kernel_size_t;
typedef intptr_t __kernel_ssize_t;
typedef intptr_t __kernel_long_t;
typedef uintptr_t __kernel_ulong_t;

typedef __u64 __le64;
typedef __u32 __le32;
typedef __u16 __le16;

#endif /* _LINUX_TYPES_H */

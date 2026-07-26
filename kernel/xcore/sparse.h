/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_SPARSE_H
#define KERNEL_SPARSE_H

#include <stdint.h>

#ifdef __CHECKER__
#define __user __attribute__((noderef, address_space(1)))
#define __iomem __attribute__((noderef, address_space(2)))
#define __force __attribute__((force))
#define __bitwise __attribute__((bitwise))
#define __acquires(x) __attribute__((context(x, 0, 1)))
#define __releases(x) __attribute__((context(x, 1, 0)))
#define __must_check __attribute__((warn_unused_result))
#else
#define __user
#define __iomem
#define __force
#define __bitwise
#define __acquires(x)
#define __releases(x)
#define __must_check
#endif

// __maybe_unused: silence -Wunused-function/-Wunused-variable on entities that
// are deliberately kept (e.g. register-accessor stubs in an in-progress
// driver). gcc already tolerates unused static inline; clang warns, so this is
// needed for the clang build. Mirrors the Linux kernel annotation.
#define __maybe_unused __attribute__((unused))

// ===================== Strong address-space types =====================
typedef uint64_t __bitwise phys_addr_t;  // physical address
typedef uint64_t __bitwise kern_vaddr_t; // kernel virtual address

#endif // KERNEL_SPARSE_H

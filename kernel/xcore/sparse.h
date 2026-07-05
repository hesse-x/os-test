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

// ===================== Strong address-space types =====================
typedef uint64_t __bitwise phys_addr_t;  // physical address
typedef uint64_t __bitwise kern_vaddr_t; // kernel virtual address

#endif // KERNEL_SPARSE_H

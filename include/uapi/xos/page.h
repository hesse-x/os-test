/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * User-visible page constants — the UAPI subset of arch/x64/memlayout.h.
 * Published to the sysroot (unlike arch/), so user code (libc, libdrm
 * consumers, Mesa) can resolve <sys/param.h>'s PAGE_SIZE without pulling
 * in kernel-internal layout. arch/x64/memlayout.h includes this header to
 * keep a single source of truth for the page size.
 */

#ifndef _XOS_PAGE_H
#define _XOS_PAGE_H

#define PAGE_SHIFT 12
#define PAGE_SIZE (1 << PAGE_SHIFT) /* 4096 */
#define PAGE_SIZE_2M 0x200000

#endif /* _XOS_PAGE_H */

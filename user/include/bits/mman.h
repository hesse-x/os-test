/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * musl <arch>/bits/mman.h equivalent for this OS. musl's <sys/mman.h> pulls
 * <bits/mman.h> at the end; on -I order user/include precedes musl, so this
 * file is the one musl's header resolves to (NOT musl's
 * arch/x86_64/bits/mman.h, which carries only MAP_32BIT). It must therefore
 * supply BOTH:
 *
 *   (1) the arch/x86_64 bit musl's generic <sys/mman.h> does NOT define itself
 *       (MAP_32BIT), and
 *   (2) the OS-specific constants the kernel honours (include/uapi/xos/mman.h)
 *       that musl's generic <sys/mman.h> also omits: MAP_FIXED_NOREPLACE,
 *       MAP_SHARED_VALIDATE, MAP_GROWSUP, PROT_SEM, and the memfd_create flags
 *       MFD_CLOEXEC / MFD_ALLOW_SEALING.
 *
 * musl's generic <sys/mman.h> already defines the common set (MAP_SHARED/
 * PRIVATE/FIXED/ANON/HUGETLB/POPULATE/STACK/LOCKED/NORESERVE/GROWSDOWN,
 * PROT_READ/WRITE/EXEC/NONE/GROWSDOWN/GROWSUP, MREMAP_MAYMOVE/FIXED, MS_*,
 * MCL_*, MADV_*, POSIX_MADV_*); those are NOT redefined here (guarded against
 * accidental double-define by #ifndef).
 *
 * Cross-consistency with the kernel UAPI (include/uapi/xos/mman.h) is enforced
 * by the static_assert block at the bottom — values must match bit-for-bit.
 * install-headers.sh publishes this file to the sysroot bits/mman.h.
 */
#ifndef _BITS_MMAN_H
#define _BITS_MMAN_H

/* arch/x86_64: the one arch bit musl's generic <sys/mman.h> lacks. */
#ifndef MAP_32BIT
#define MAP_32BIT 0x40
#endif

/* OS-specific mmap flags honoured by the kernel (xos/mman.h). musl's generic
 * <sys/mman.h> does not define these. */
#ifndef MAP_SHARED_VALIDATE
#define MAP_SHARED_VALIDATE                                                    \
  0x03 /* Linux 4.15+; kernel treats as MAP_SHARED                             \
        */
#endif
#ifndef MAP_GROWSUP
#define MAP_GROWSUP 0x200 /* no stack-grow mechanism; accepted as no-op */
#endif
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

/* PROT_SEM: accepted by the kernel's mprotect (masked to no-op). musl's
 * generic <sys/mman.h> does not define it. */
#ifndef PROT_SEM
#define PROT_SEM 0x10
#endif

/* memfd_create flags (xos/mman.h). musl's <sys/mman.h> has no memfd_create,
 * so neither the declaration nor its flags come from musl. */
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif

/* ---- compile-time consistency with the kernel UAPI (include/uapi/xos/mman.h)
 * ---- The OS-specific constants above MUST stay bit-for-bit identical to the
 * kernel's UAPI contract in include/uapi/xos/mman.h (the kernel reads the
 * raw flags userspace passes to mmap/mprotect/memfd_create). xos/mman.h is NOT
 * included here: it also defines the common set (MAP_SHARED/PROT_READ/...) that
 * musl's <sys/mman.h> already defined before reaching <bits/mman.h>, so pulling
 * it in would trip -Wmacro-redefined. Cross-parity is instead enforced by a
 * separate compile-time guard TU, kernel/bsd/mman_sync.c (mirroring the
 * fcntl_sync.c discipline): it captures these bits-side values, clears the
 * macros, includes xos/mman.h, and _Static_asserts each matches. A drift
 * there fails the kernel build; the values below are the source of truth for
 * the userspace side. */

#endif /* _BITS_MMAN_H */

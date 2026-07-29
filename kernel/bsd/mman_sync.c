/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * Compile-time cross-consistency guard between the userspace musl <sys/mman.h>
 * (+ repo bits/mman.h) and the kernel shared UAPI include/uapi/xos/mman.h
 * (musl_worklist mman module, step 4).
 *
 * After the mman module switched to musl, the userspace libc consumes musl
 * <sys/mman.h> (the common MAP_xx / PROT_xx set) plus the repo user/include/
 * bits/mman.h (the OS-specific MAP_FIXED_NOREPLACE / MAP_SHARED_VALIDATE /
 * MAP_GROWSUP / PROT_SEM / MFD_xx / MAP_32BIT that musl generic header lacks).
 * The kernel reads the raw flags userspace passes to mmap / mprotect /
 * memfd_create straight out of xos/mman.h. A silent drift in any of these
 * OS-specific values between bits/mman.h and xos/mman.h would corrupt the
 * kernel-to-user ABI (e.g. the kernel masking the wrong bit, or memfd_create
 * flags misread).
 *
 * This TU forces them to match at compile time: it pulls the repo bits/mman.h
 * (pure macros), captures every OS-specific constant into an enum, clears the
 * macros, then includes xos/mman.h and _Static_asserts each captured value
 * equals the kernel. It does NOT pull musl full <sys/mman.h> (its MAP_SHARED /
 * PROT_READ would collide with xos/mman.h); only the macro-only bits/mman.h
 * is included.
 *
 * The common set (MAP_SHARED / PROT_READ / MAP_FAILED / ...) is NOT asserted
 * here: both musl generic <sys/mman.h> and xos/mman.h hard-code the Linux
 * x86-64 values, and xos/mman.h is the kernel own frozen UAPI, so those agree
 * by construction (and pulling musl header here would redefine them against
 * xos/mman.h). Only the OS-specific extensions bits/mman.h adds are at risk
 * of drift, so only those are asserted.
 *
 * This TU produces no code (every assertion is compile-time); it is built as
 * the mman_sync_check OBJECT library and merged into the kernel link purely so
 * a drift fails the build.
 */

/* 1. Pull the repo bits/mman.h — pure MAP_xx / PROT_xx / MFD_xx macros, no
 *    struct. Resolved via the mman_sync_check include dirs (user/include +
 *    include/uapi ahead of musl, matching the libc build order). */
#include <bits/mman.h> // IWYU pragma: keep

/* 2. Capture the OS-specific constant values into an enum (K(x) -> KSYNC_x).
 *    Only the subset xos/mman.h ALSO defines is asserted below. MAP_32BIT is
 *    NOT captured: it is a musl arch/x86_64 bit (not an OS UAPI constant), so
 *    xos/mman.h does not define it and there is nothing to assert it against.
 */
#define K(x) KSYNC_##x = (x),
enum {
  K(MAP_SHARED_VALIDATE) K(MAP_GROWSUP) K(MAP_FIXED_NOREPLACE) K(PROT_SEM)
      K(MFD_CLOEXEC) K(MFD_ALLOW_SEALING)
};
#undef K

/* 3. Clear every macro bits/mman.h defined (that we captured) so xos/mman.h
 *    can redefine the shared subset without -Wmacro-redefined. MAP_32BIT is
 *    left defined — xos/mman.h does not redefine it, and clearing it would
 *    only drop a musl arch bit this TU does not use afterward. */
#undef MAP_SHARED_VALIDATE
#undef MAP_GROWSUP
#undef MAP_FIXED_NOREPLACE
#undef PROT_SEM
#undef MFD_CLOEXEC
#undef MFD_ALLOW_SEALING

/* 4. Now pull the kernel UAPI definitions. */
#include <xos/mman.h>

/* 5. Assert every OS-specific constant matches the kernel UAPI. A drift here
 *    fails the build (musl_worklist mman step 4). */
#define CK(x)                                                                  \
  _Static_assert(KSYNC_##x == (x), #x " bits/mman.h vs xos/mman.h mismatch")
CK(MAP_SHARED_VALIDATE);
CK(MAP_GROWSUP);
CK(MAP_FIXED_NOREPLACE);
CK(PROT_SEM);
CK(MFD_CLOEXEC);
CK(MFD_ALLOW_SEALING);
#undef CK

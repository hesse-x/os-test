/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef COMMON_PRCTL_H
#define COMMON_PRCTL_H

// Linux-compatible prctl options. Only the subset used by third-party code
// (Wayland, LLVM) and our internal callers is defined; unimplemented options
// return -EINVAL.

#define PR_SET_PDEATHSIG 1        // (stub: no-op)
#define PR_GET_PDEATHSIG 2        // (stub: return 0)
#define PR_GET_DUMPABLE 3         // (stub: return 1)
#define PR_SET_DUMPABLE 4         // (stub: no-op, accept 1/0)
#define PR_GET_NAME 16            // read task comm[16]
#define PR_SET_NAME 15            // write task comm[16]
#define PR_SET_PTRACER 0x59616d61 // (stub: no-op)
#define PR_GET_AUXV 0x41555856 // (stub: return -EINVAL; AUXV is arch-specific)
#define PR_SET_VMA 0x53564d41
#define PR_SET_VMA_ANON_NAME 0

/* XOS service profiles: GET reads xos_cap_state; SET can only remove bits. */
#define PR_XOS_CAP_GET 0x58504301
#define PR_XOS_CAP_SET 0x58504302

#endif /* COMMON_PRCTL_H */

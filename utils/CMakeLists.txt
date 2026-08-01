# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

# utils/ contains non-UAPI shared implementation only:
#   macro.h        — ALIGN_UP/ALIGN_DOWN generic macros (used by kernel + user malloc)
#   kvformat.c/.h — shared printf formatting impl (compiled by both kernel and libc; NOT a UAPI contract)
# UAPI contract headers (syscall, errno, stat, shm, ...) live in xos/ — see xos/CMakeLists.txt.
# No compilation targets here — all headers are header-only (kvformat.c is compiled via add_*
# rules in the kernel/user CMake trees that include it).
#
# A single shared STATIC target for os_utils was investigated (phases 1-3) and is
# infeasible — kvformat.c's %f branch uses va_arg(ap,double) + double arithmetic,
# which depends on the SSE ABI (userspace). The kernel builds with -D__KERNEL__
# -mno-sse, so that branch is #ifdef'd out and the kernel object has no SSE.
# A single STATIC object serving both either drops %f in userspace (built
# -mno-sse) or links illegal SSE into the kernel (built with SSE). The objects
# cannot be merged. The current deliberate dual-compile (kernel/xcore builds the
# kernel copy, user/libc builds the userspace copy) is the correct handling and
# stays. Open item #3 closes on this conclusion: os_utils has no single object;
# sharing would require deriving two object sets by flag set (as libc.a/so does),
# out of scope for phase 3.

# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

# utils/ contains non-UAPI shared implementation only:
#   macro.h        — ALIGN_UP/ALIGN_DOWN generic macros (used by kernel + user malloc)
#   kvformat.c/.h — shared printf formatting impl (compiled by both kernel and libc; NOT a UAPI contract)
# UAPI contract headers (syscall, errno, stat, shm, ...) live in xos/ — see xos/CMakeLists.txt.
# No compilation targets here — all headers are header-only (kvformat.c is compiled via add_*
# rules in the kernel/user CMake trees that include it).

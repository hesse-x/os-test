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
# os_utils 共享 STATIC target — 经阶段 1-3 验证，结论为"不可建单一对象"（reface_cmake.md
# §4.7 / 附录待决项 #3）：
#   kvformat.c 的 %f 分支用 va_arg(ap,double) + double 算术，依赖 SSE ABI（用户态）；
#   内核构建带 -D__KERNEL__ -mno-sse，该分支被 #ifdef 排除，故内核对象不含 SSE 指令。
#   若用单一 STATIC 对象服务两者：-mno-sse 编译则用户态缺 %f；开 SSE 编译则内核链接进
#   非法 SSE 指令。两组对象不可合并。当前故意双编译（kernel/xcore 编内核副本，
#   user/libc 编用户态副本）正是对该分歧的正确处理，保留不动。
#   待决项 #3 以本结论关闭：os_utils 不建单一对象；如需共享，需像 libc.a/so 那样按
#   flag 集派生两组对象，超出阶段 3 范围。

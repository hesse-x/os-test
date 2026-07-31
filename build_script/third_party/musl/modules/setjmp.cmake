# modules/setjmp.cmake — musl setjmp integration (musl_worklist).
# Included by musl_rules.cmake after musl_generate_headers() + set(MUSL_DIR).
# Expects: MUSL_DIR, add_musl_lib.
# ===================== musl setjmp integration =====================
# Build the upstream musl setjmp/longjmp into libc, replacing the repo's
# hand-written user/lib/setjmp.S (deleted this batch). The <setjmp.h> header
# switches to musl's (repo user/include/setjmp.h deleted; install-headers §3o
# publishes musl's setjmp.h + bits/setjmp.h).
#
# musl's layout: src/setjmp/{setjmp,longjmp}.c are EMPTY (0 bytes) — the real
# implementations are the arch asm src/setjmp/x86_64/{setjmp,longjmp}.s, which
# musl's Makefile substitutes for the .c twins via its ARCH_GLOBS/REPLACED_OBJS
# rule (src/setjmp/$(ARCH)/*.s replaces src/setjmp/*.c). We do the same by hand:
# compile ONLY the x86_64 .s pair (the empty .c would build to nothing). The
# setjmp.s defines __setjmp/_setjmp/setjmp as aliases of one body; longjmp.s
# defines _longjmp/longjmp — all five symbols land in libc, no C glue needed.
#
# ABI compatibility (why swapping the repo asm for musl's is safe): both save
# the same 8 callee-saved registers (rbx rbp r12 r13 r14 r15 rsp rip) into an
# 8-element buffer. The repo's jmp_buf was `long long[8]`; musl's is
# `struct __jmp_buf_tag { __jmp_buf __jb; unsigned long __fl; unsigned long
# __ss[128/8]; }[1]` where __jb is `unsigned long[8]` (arch/x86_64/bits/
# setjmp.h). The asm only touches __jb[0..7] (the first 64 bytes), leaving
# __fl/__ss (the signal-mask save area, used only by sigsetjmp) untouched —
# so plain setjmp/longjmp are binary-identical in behavior. The lone in-tree
# consumer is Unity (TEST_PROTECT=setjmp / TEST_ABORT=longjmp on
# `jmp_buf AbortFrame`), which is type-abstract over the layout, so the struct
# change is transparent to it.
#
# sigsetjmp: musl's src/signal/sigsetjmp.c is also empty (the real sigsetjmp is
# the arch asm, which musl builds under src/signal/x86_64/ — but that dir does
# NOT exist in musl 1.2.6; sigsetjmp is supplied by the setjmp asm via a
# separate entry point only on arches that define it, and x86_64's setjmp.s
# does NOT define sigsetjmp). So sigsetjmp has NO musl implementation on
# x86_64 — it stays declare-only in <setjmp.h> (gated _POSIX/_XOPEN), 0
# in-tree callers. siglongjmp IS built (musl_signal_objs, src/signal/
# siglongjmp.c — it just calls longjmp); it is exported but its declared
# pair sigsetjmp is not, a known half-finished state carried over from the
# signal tier (doc/design/todo.md). This module adds nothing for sigsetjmp.
#
# No multi-define: nothing else builds setjmp/longjmp once setjmp.S is deleted.
# No locale/syscall dependency — pure register save/restore. Single -fPIC
# OBJECT lib via add_musl_lib (same one-compile-serves-both pattern as ctype),
# wired into libc.a + libc.so via EXTRA_OBJS in user/CMakeLists.txt.
add_musl_lib(musl_setjmp_objs SOURCES
    ${MUSL_DIR}/src/setjmp/x86_64/setjmp.s
    ${MUSL_DIR}/src/setjmp/x86_64/longjmp.s)

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _SETJMP_H
#define _SETJMP_H

#include <stddef.h>
#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* jmp_buf: saves 8 callee-saved registers (rbx rbp r12 r13 r14 r15 rsp rip).
 * Defined as long long[8] to guarantee 8-byte alignment. rip stores the
 * return address right after the setjmp call site; longjmp resumes from
 * there. Signal mask is not saved (C standard setjmp does not require it;
 * sigsetjmp/siglongjmp see todo.md). */
typedef long long jmp_buf[8];

LIBC_EXPORT int setjmp(jmp_buf env);
LIBC_EXPORT void longjmp(jmp_buf env, int val) __attribute__((noreturn));

/* POSIX aliases (no underscore) */
LIBC_EXPORT int _setjmp(jmp_buf env);
LIBC_EXPORT void _longjmp(jmp_buf env, int val) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* _SETJMP_H */

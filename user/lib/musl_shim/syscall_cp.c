/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * musl unistd pthread-mechanism downgrade shim.
 *
 * musl's unistd sources reach into the pthread subsystem in two ways:
 *   - cancellable wrappers (read/write/fsync/pause/...) call the macro
 *     syscall_cp(...) which expands to __syscall_ret(__syscall_cp(...)).
 *     The real musl __syscall_cp (src/thread/__syscall_cp.c) routes through
 *     __syscall_cp_asm (src/thread/x86_64/syscall_cp.s) and the pthread_cancel
 *     machinery to interrupt an in-flight syscall. This OS's self-authored
 *     pthread (user/lib/pthread.cc) does NOT implement pthread_cancel, so the
 *     cancellation point is dropped: __syscall_cp here performs a plain,
 *     non-cancellable syscall via the inline __syscall6 arch stub. Behaviour is
 *     identical to a normal blocking syscall — which is what the OS already
 * had.
 *   - set*id wrappers (setuid/setgid/seteuid/.../setresuid/setresgid, 8 files)
 *     call __setxid(nr,id,eid,sid). The real musl __setxid
 * (src/unistd/setxid.c) broadcasts the id change to every thread via __synccall
 * and SIGKILLs the process if any thread fails. This kernel's credentials are
 * process-wide (not Linux per-thread creds), and user/lib/sys_process.cc:84
 * already implements the set*id family as direct syscalls with no __synccall.
 * The
 *     __setxid here mirrors that: a single direct syscall, errno-translated the
 *     musl way. Multi-threaded processes keep stale creds on other threads
 * until their next syscall — identical to the existing repo behaviour.
 *
 * This file is compiled with the musl-internal include order (musl src/internal
 * before user/include) so "syscall.h"/"libc.h" resolve to musl's own headers,
 * exactly as the musl unistd .c files see them. It must NOT depend on musl's
 * variadic __syscall asm stub (src/internal/x86_64/syscall.s) nor on any musl
 * thread source — __syscall6 is an inline arch stub from syscall_arch.h.
 *
 * We do NOT compile musl's src/unistd/setxid.c, src/thread/__syscall_cp.c,
 * src/thread/x86_64/syscall_cp.s, src/thread/synccall.c, nor
 * src/thread/pthread_cancel.c: this shim replaces all of them.
 */
#include "libc.h"
#include "syscall.h"

/* musl's internal/syscall.h defines __syscall_cp as an *overload macro*
 * (__syscall_cp1/2/.../6) that the unistd wrappers use. Each arm ends in
 * (__syscall_cp)(n, ...): the parenthesised name suppresses macro expansion,
 * so it refers to the *function* __syscall_cp, not the macro (confirmed:
 * read.o has an undefined ref U __syscall_cp). Undefine the macro here so the
 * function definition below is not mangled, then define the function. */
#undef __syscall_cp

/* Non-cancellable cancellable-syscall trampoline.
 * Equivalent to musl's sccp() but using the inline 6-arg arch stub so no
 * variadic __syscall symbol is referenced. Returns the RAW syscall value —
 * the caller (musl's syscall_cp macro = __syscall_ret(__syscall_cp(...)),
 * or close.c's explicit __syscall_ret(r)) applies __syscall_ret exactly once.
 * Wrapping here would double-map: inner __syscall_ret(-EAGAIN) → errno=11,
 * returns -1; outer __syscall_ret(-1) → errno=EPERM(1), clobbering the real
 * errno. Mirrors real musl __syscall_cp (raw __syscall_cp_asm return). */
long __syscall_cp(syscall_arg_t nr, syscall_arg_t a0, syscall_arg_t a1,
                  syscall_arg_t a2, syscall_arg_t a3, syscall_arg_t a4,
                  syscall_arg_t a5) {
  return __syscall6(nr, a0, a1, a2, a3, a4, a5);
}

/* Process-wide set*id without cross-thread broadcast (see file header).
 * Maps directly to the kernel's process-wide setresuid/setresgid/etc. */
int __setxid(int nr, int id, int eid, int sid) {
  return __syscall_ret(__syscall3(nr, id, eid, sid));
}

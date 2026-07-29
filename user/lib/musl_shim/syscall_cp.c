/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * musl unistd __setxid shim.
 *
 * musl's set*id wrappers (setuid/setgid/seteuid/.../setresuid/setresgid, 8
 * files in src/unistd) call __setxid(nr,id,eid,sid). The real musl __setxid
 * (src/unistd/setxid.c) broadcasts the id change to every thread via __synccall
 * and SIGKILLs the process if any thread fails. __synccall walks
 * /proc/self/task (procfs), which this kernel does not have, so synccall.c is
 * excluded from musl_pthread and musl's setxid.c is excluded from
 * musl_unistd_objs. This kernel's credentials are process-wide (not Linux
 * per-thread creds), so the cross-thread broadcast is unnecessary anyway:
 * __setxid here performs a single direct syscall, errno-translated the musl
 * way — identical to the existing repo behaviour (user/lib/sys_process.cc).
 *
 * The OTHER musl-unistd/pthread coupling — cancellable wrappers (read/write/
 * fsync/pause/...) calling __syscall_cp — is now served by musl's REAL
 * __syscall_cp (src/thread/__syscall_cp.c + x86_64/syscall_cp.s), compiled into
 * musl_pthread. musl pthread implements pthread_cancel, so the cancellation
 * point is live (no longer downgraded to a non-cancellable syscall as when the
 * repo carried its own pthread). This shim therefore no longer defines
 * __syscall_cp — doing so would duplicate musl_pthread's symbol at link.
 *
 * Compiled with the musl-internal include order (musl src/internal before
 * user/include) so "syscall.h"/"libc.h" resolve to musl's own headers, exactly
 * as the musl unistd .c files see them. __syscall3 is an inline arch stub from
 * syscall_arch.h; __syscall_ret is provided by musl_pthread's
 * src/internal/syscall_ret.c at link time.
 */
#include "libc.h"
#include "syscall.h"

/* Process-wide set*id without cross-thread broadcast (see file header).
 * Maps directly to the kernel's process-wide setresuid/setresgid/etc. */
int __setxid(int nr, int id, int eid, int sid) {
  return __syscall_ret(__syscall3(nr, id, eid, sid));
}

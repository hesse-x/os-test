/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * Compile-time cross-consistency guard between the userspace musl <fcntl.h>
 * and the kernel-private kernel/bsd/kcntl.h (fcntl_worklist §3c).
 *
 * After the fcntl header split (§3b) the userspace libc consumes musl's real
 * <fcntl.h> (O_*, F_*, struct flock from musl), while the kernel consumes its
 * own kernel/bsd/kfcntl.h. The two no longer share a UAPI header, so a silent
 * drift in constant values or struct layout would corrupt the kernel↔user ABI
 * (e.g. the kernel reading a struct flock the user filled with musl's layout,
 * or UTIME_NOW/OMIT mapping to the wrong tv_nsec). This TU forces them to match
 * at compile time: it pulls musl's bits/fcntl.h (pure macros, no struct),
 * captures every shared constant into an enum, clears musl's macros, then
 * includes kernel/bsd/kfcntl.h and _Static_asserts each captured value equals
 * the kernel's. It also locks struct flock / f_owner_ex to the Linux x86-64
 * layout and pins UTIME_NOW/UTIME_OMIT to the Linux/musl literals (the repo's
 * old shared header had them swapped — this guards against a regression).
 *
 * This TU produces no code (every assertion is compile-time); it is built as
 * the fcntl_sync_check OBJECT library and merged into the kernel link purely so
 * a drift fails the build. Built as a kernel TU (musl bits + kernel kfcntl.h on
 * the include path; musl's full <fcntl.h> is NOT pulled here because its struct
 * flock would collide with kfcntl.h's — only the macro-only bits/fcntl.h is).
 */

/* 1. Pull musl's bits/fcntl.h (arch/x86_64) — pure O_*, F_* macros, no struct.
 *    Resolved via the fcntl_sync_check include dirs (musl/include +
 *    musl/arch/x86_64) ahead of any repo path. */
#include <bits/fcntl.h> // IWYU pragma: keep
#include <stddef.h>

/* 2. Capture musl's constant values into an enum (K(x) -> KSYNC_x = (x)). Only
 *    the subset that kernel/bsd/kcntl.h ALSO defines is asserted below; the
 *    musl-only macros (O_RSYNC/O_ASYNC/O_NOATIME/O_NDELAY/F_GETOWNER_UIDS) are
 *    not asserted (kfcntl.h doesn't define them). */
#define K(x) KSYNC_##x = (x),
enum {
  K(O_CREAT) K(O_EXCL) K(O_NOCTTY) K(O_TRUNC) K(O_APPEND) K(O_NONBLOCK)
      K(O_DSYNC) K(O_SYNC) K(O_DIRECTORY) K(O_NOFOLLOW) K(O_CLOEXEC) K(O_DIRECT)
          K(O_LARGEFILE) K(O_PATH) K(O_TMPFILE) K(F_DUPFD) K(F_GETFD) K(F_SETFD)
              K(F_GETFL) K(F_SETFL) K(F_SETOWN) K(F_GETOWN) K(F_SETSIG)
                  K(F_GETSIG) K(F_GETLK) K(F_SETLK) K(F_SETLKW) K(F_SETOWN_EX)
                      K(F_GETOWN_EX)
};
#undef K

/* 3. Clear every macro musl's bits/fcntl.h defined (including the musl-only
 *    ones) so kernel/bsd/kcntl.h can redefine the shared subset without
 *    -Wmacro-redefined. */
#undef O_CREAT
#undef O_EXCL
#undef O_NOCTTY
#undef O_TRUNC
#undef O_APPEND
#undef O_NONBLOCK
#undef O_DSYNC
#undef O_SYNC
#undef O_RSYNC
#undef O_DIRECTORY
#undef O_NOFOLLOW
#undef O_CLOEXEC
#undef O_ASYNC
#undef O_DIRECT
#undef O_LARGEFILE
#undef O_NOATIME
#undef O_PATH
#undef O_TMPFILE
#undef O_NDELAY
#undef F_DUPFD
#undef F_GETFD
#undef F_SETFD
#undef F_GETFL
#undef F_SETFL
#undef F_SETOWN
#undef F_GETOWN
#undef F_SETSIG
#undef F_GETSIG
#undef F_GETLK
#undef F_SETLK
#undef F_SETLKW
#undef F_SETOWN_EX
#undef F_GETOWN_EX
#undef F_GETOWNER_UIDS

/* 4. Now pull the kernel's own definitions (macros + struct flock + struct
 *    f_owner_ex + UTIME_*). */
#include "kernel/bsd/kfcntl.h"

/* 5. Assert every shared constant matches musl. A drift here fails the build
 *    (fcntl_worklist §3c). */
#define CK(x) _Static_assert(KSYNC_##x == (x), #x " musl vs kernel mismatch")
CK(O_CREAT);
CK(O_EXCL);
CK(O_NOCTTY);
CK(O_TRUNC);
CK(O_APPEND);
CK(O_NONBLOCK);
CK(O_DSYNC);
CK(O_SYNC);
CK(O_DIRECTORY);
CK(O_NOFOLLOW);
CK(O_CLOEXEC);
CK(O_DIRECT);
CK(O_LARGEFILE);
CK(O_PATH);
CK(O_TMPFILE);
CK(F_DUPFD);
CK(F_GETFD);
CK(F_SETFD);
CK(F_GETFL);
CK(F_SETFL);
CK(F_SETOWN);
CK(F_GETOWN);
CK(F_SETSIG);
CK(F_GETSIG);
CK(F_GETLK);
CK(F_SETLK);
CK(F_SETLKW);
CK(F_SETOWN_EX);
CK(F_GETOWN_EX);
#undef CK

/* 6. Lock struct flock / struct f_owner_ex to the Linux x86-64 layout (the
 *    exact layout musl's <fcntl.h> also produces — musl≡Linux frozen ABI).
 *    Redundant with kfcntl.h's own asserts but explicit here as the
 *    kernel↔user contract. */
_Static_assert(offsetof(struct flock, l_type) == 0, "flock.l_type");
_Static_assert(offsetof(struct flock, l_start) == 8, "flock.l_start");
_Static_assert(offsetof(struct flock, l_len) == 16, "flock.l_len");
_Static_assert(offsetof(struct flock, l_pid) == 24, "flock.l_pid");
_Static_assert(sizeof(struct flock) == 32, "flock size (x86-64)");
_Static_assert(offsetof(struct f_owner_ex, type) == 0, "f_owner_ex.type");
_Static_assert(offsetof(struct f_owner_ex, pid) == 4, "f_owner_ex.pid");
_Static_assert(sizeof(struct f_owner_ex) == 8, "f_owner_ex size");

/* 7. Pin UTIME_NOW/UTIME_OMIT to the Linux/musl literals. The repo's old shared
 *    xos/fcntl.h had these swapped (NOW=(1<<30)-2, OMIT=(1<<30)-1); masked
 *    because kernel+userspace shared the same wrong values, but the split (musl
 *    userspace uses the correct values) would have made do_utimensat interpret
 *    them backwards. Fixed in kfcntl.h; this assert prevents regression. */
_Static_assert(UTIME_NOW == 0x3fffffff, "UTIME_NOW must match Linux/musl");
_Static_assert(UTIME_OMIT == 0x3ffffffe, "UTIME_OMIT must match Linux/musl");

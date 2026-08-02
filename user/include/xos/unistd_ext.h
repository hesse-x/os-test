/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * OS-specific unistd extensions not covered by the musl <unistd.h>.
 *
 * musl's <unistd.h> is now the file-level replacement for the legacy
 * user/include/unistd.h (plan §4.5). The handful of declarations that existed
 * in the old header but have no counterpart in musl's <unistd.h> — and do not
 * belong to another standard header — live here. Consumers add
 * #include <xos/unistd_ext.h> after <unistd.h>.
 *
 * Functions that musl places in a different header are declared there instead:
 *   sched_yield  → <sched.h>            (repo already declares it)
 *   memfd_create → <sys/mman.h>          (repo already declares it)
 *   umask        → <sys/stat.h>          (added there)
 *   utimensat    → <sys/stat.h>          (added there)
 *   ioperm       → <sys/io.h>            (added there, matches musl layout)
 *   _exit        → <unistd.h>            (musl declares it directly)
 *
 * Also re-declared here with LIBC_EXPORT: functions that musl's <unistd.h>
 * *does* declare but either (a) only under a feature-test macro the libc
 * build does not enable (getpagesize, sethostname sit behind _GNU_SOURCE/
 * _BSD_SOURCE in musl unistd.h, so the C++ definition in our libc would be
 * name-mangled with no prior C declaration), or (b) unconditionally but
 * without any visibility attribute (gethostname, sysconf), so under
 * -fvisibility=hidden they'd compile to HIDDEN and fail to export. The
 * LIBC_EXPORT here gives each a visible C-linkage declaration that the
 * matching definition (now in uname.c / stdlib_misc.c) picks up.
 */
#ifndef _XOS_UNISTD_EXT_H
#define _XOS_UNISTD_EXT_H

#include <sys/cdefs.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Thread ID of the calling thread (Linux gettid(2)). musl has no public
 * declaration; this OS exposes it directly. Defined in sys_process.cc. */
LIBC_EXPORT pid_t gettid(void);

/* Block until the named device node has been registered by its driver.
 * OS-specific init-order helper used by user-space drivers.
 * Defined in sys_device.cc. */
LIBC_EXPORT void wait_dev_ready(const char *dev_path);

/* Hostname get/set. musl unistd.h declares gethostname unconditionally but
 * without a visibility attribute (→ HIDDEN under -fvisibility=hidden); musl's
 * src/unistd/gethostname.c is adopted (reads uname.nodename), so no repo
 * definition and no re-export needed. sethostname is declared by musl only
 * under _GNU_SOURCE/_BSD_SOURCE (not enabled in the libc build → no prior C
 * declaration → C++ mangling); re-declared here with LIBC_EXPORT so musl's
 * src/linux/sethostname.c definition exports cleanly. */
LIBC_EXPORT int sethostname(const char *name, size_t len);

/* Page size. musl unistd.h declares this only under _GNU_SOURCE/_BSD_SOURCE
 * (not enabled in the libc build), so re-declare with LIBC_EXPORT. Defined
 * in stdlib_misc.c. */
LIBC_EXPORT int getpagesize(void);

/* sysconf(3). musl unistd.h declares this unconditionally but without a
 * visibility attribute (→ HIDDEN). Re-declared with LIBC_EXPORT; the _SC_*
 * constants still come from musl's <unistd.h>. Defined in stdlib_misc.c. */
LIBC_EXPORT long sysconf(int name);

#ifdef __cplusplus
}
#endif

#endif /* _XOS_UNISTD_EXT_H */

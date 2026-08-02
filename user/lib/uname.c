/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include <xos/unistd_ext.h>

#include <xos/syscall_ext.h>

/* uname() is now provided by musl src/misc/uname.c (musl_misc_objs), which
 * calls syscall(SYS_uname) and fills the 6-field struct utsname from the
 * kernel's sys_uname (kernel/bsd/syscall.c) — sysname/nodename/release/version
 * /machine/domainname. The kernel reads the live hostname into nodename, so
 * uname().nodename tracks sethostname(). The repo's old hand-coded uname()
 * (hard-coded strings, never called the syscall) is removed. */

/* gethostname is RETAINED (musl src/unistd/gethostname.c is excluded from
 * musl_unistd_objs — see user/CMakeLists.txt): musl's gethostname returns
 * uname().nodename but TRUNCATES to len and returns 0 even when the buffer is
 * too small, whereas this repo's gethostname routes to the kernel's
 * sys_gethostname, which returns -1/EINVAL on a too-small buffer — the
 * test_process.c hostname round-trip (case "buffer too small returns -1 +
 * EINVAL") depends on this stricter kernel-side check. sethostname comes from
 * musl src/linux/sethostname.c (musl_linux_objs, routes to SYS_sethostname).
 * The LIBC_EXPORT re-declaration in <xos/unistd_ext.h> gives this definition
 * C linkage + default visibility (musl's <unistd.h> declares gethostname
 * without a visibility attribute → HIDDEN under -fvisibility=hidden). */
int gethostname(char *name, size_t len) { return sys_gethostname(name, len); }

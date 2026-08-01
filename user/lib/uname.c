/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include <sys/utsname.h>
#include <xos/unistd_ext.h>

#include <syscall.h>

int uname(struct utsname *buf) {
  if (!buf)
    return -1;
  strcpy(buf->sysname, "Xos");
  strcpy(buf->nodename, "(none)");
  strcpy(buf->release, "0.1");
  strcpy(buf->version, __DATE__);
  strcpy(buf->machine, "x86_64");
  return 0;
}

/* gethostname is RETAINED (musl src/unistd/gethostname.c is excluded from
 * musl_unistd_objs — see user/CMakeLists.txt): musl's version returns
 * uname().nodename, but this repo's uname hard-codes nodename="(none)", so
 * musl's gethostname would ignore sethostname() and always read "(none)".
 * The repo version reads the kernel's live hostname (sys_gethostname),
 * which sethostname() updates — the test_process.c hostname round-trip
 * depends on this. sethostname now comes from musl src/linux/sethostname.c
 * (musl_linux_objs, routes to SYS_sethostname), so it is no longer defined
 * here. The LIBC_EXPORT re-declaration in <xos/unistd_ext.h> gives this
 * definition C linkage + default visibility (musl's <unistd.h> declares
 * gethostname without a visibility attribute → HIDDEN under
 * -fvisibility=hidden). */
int gethostname(char *name, size_t len) { return sys_gethostname(name, len); }

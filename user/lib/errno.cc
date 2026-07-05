/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// user/lib/errno.cc — per-thread errno via TCB
//
// errno is returned via __errno_location() as &TCB.errno_val (FS_BASE points at the
// TCB), independent per thread.
//
// This solves the dynamic-linking issue where R_X86_64_COPY copies libc.so's errno
// into the main ELF's .bss, leaving the main ELF and libc.so each holding their own
// errno with split read/write (a failed syscall inside libc.so writes its own .bss
// errno, while the main ELF reads its own COPY copy and always gets 0).
//
// TCB model: errno_val lives in the TCB (pointed to by FS_BASE); both the main ELF
// and libc.so access the current thread's single errno via %fs:offset. No __thread
// needed (avoids generating general dynamic TLS relocations such as
// R_X86_64_DTPMOD64; ld.so does not yet implement __tls_get_addr).

#include "sys/tls.h"
#include <errno.h>

extern "C" {

int *__errno_location(void) {
  // __pthread_current_tcb() is internally `movq %fs:0, rax`; returns the TCB pointer
  struct tcb *tcb = __pthread_current_tcb();
  return &tcb->errno_val;
}

} // extern "C"

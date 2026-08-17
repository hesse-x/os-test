/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// test_dl — dlfcn API smoke test.
//
// Exercises the dlopen/dlsym/dlclose/dlerror/dladdr surface that the fused
// musl loader (ldso/dynlink.c) + the musl_dl_objs wrappers (dlsym.c/dlclose.c
// compiled into libc) provide. This is NOT a real shared-library loading test:
// dlopen(NULL, ...) returns the main-program dso head (musl semantics) without
// touching the Phase 3 load_library/TLS path, and dlsym(head, ...) resolves
// against the already-built global symbol chain (libc.so exports), which Phase
// 1.5 (ld_chain/ld_diamond) already proved works.
//
// What we verify, each step isolated so a failure points at the exact call:
//   DL-001  dlopen(NULL, RTLD_LAZY) returns non-NULL (the head handle).
//   DL-002  dlsym(head, "printf") resolves a libc global symbol (non-NULL).
//   DL-003  dlsym(head, "<bogus>") returns NULL for a missing symbol.
//   DL-004  dlerror() is musl's real implementation (src/ldso/dlerror.c in
//           musl_dl_objs, now that musl_pthread provides the struct pthread
//           dlerror_buf/dlerror_flag layout at %fs:0). After a failed dlsym the
//           loader's error() path calls __dl_vseterr, so dlerror() must return
// a non-NULL "Symbol not found: ..." string; before any failure it is NULL.
//   DL-005  dladdr(&printf, &info) returns nonzero and fills dli_fname/sname.
//   DL-006  dlclose(head) returns 0 (head is a valid handle; dlclose is a
//           no-op for the main program).
//
// Exit 0 only if every step passed; otherwise the failing step prints a marker
// and exits nonzero so test_runner reports [FAIL].

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;

  int rc = 0;

  // DL-001: dlopen(NULL) → head handle (no load).
  void *handle = dlopen(NULL, RTLD_LAZY);
  if (!handle) {
    printf("DL-001 FAIL: dlopen(NULL) returned NULL\n");
    return 1;
  }
  printf("DL-001 ok: dlopen(NULL) = %p\n", handle);

  // DL-002: resolve a real libc global symbol. printf is exported by libc.so
  // and present in the main program's global chain.
  void *printf_addr = dlsym(handle, "printf");
  if (!printf_addr) {
    printf("DL-002 FAIL: dlsym(handle,\"printf\") returned NULL\n");
    rc = 1;
  } else {
    printf("DL-002 ok: dlsym(handle,\"printf\") = %p\n", printf_addr);
  }

  // DL-003: a missing symbol must yield NULL, not crash.
  void *bogus = dlsym(handle, "__dl_test_bogus_symbol_xyz");
  if (bogus) {
    printf("DL-003 FAIL: dlsym for bogus symbol returned %p (expected NULL)\n",
           bogus);
    rc = 1;
  } else {
    printf("DL-003 ok: dlsym(bogus) = NULL\n");
  }

  // DL-004: dlerror() is musl's real implementation (dlerror.c in
  // musl_dl_objs). dlerror() consumes the pending error (clears dlerror_flag),
  // so call it once first to drain anything left by earlier steps. Then a
  // failed dlsym sets the error via the loader's error()→__dl_vseterr path, and
  // the next dlerror() must return a non-NULL "Symbol not found: ..." string.
  (void)dlerror(); // drain any pending error from DL-002/DL-003
  (void)dlsym(handle, "__dl_test_bogus_symbol_xyz");
  char *e2 = dlerror();
  if (e2 == NULL) {
    printf("DL-004 FAIL: dlerror() returned NULL after a failed dlsym "
           "(real dlerror not wired)\n");
    return 4;
  }
  printf("DL-004 ok: dlerror() after failed dlsym = \"%s\"\n", e2);

  // DL-005: dladdr on a known symbol. Use printf_addr from DL-002 (fall back to
  // our own &printf if DL-002 failed, so the step still exercises dladdr).
  Dl_info info;
  memset(&info, 0, sizeof(info));
  void *probe = printf_addr ? printf_addr : (void *)&printf;
  int ar = dladdr(probe, &info);
  if (ar == 0) {
    printf("DL-005 FAIL: dladdr returned 0\n");
    rc = 1;
  } else if (!info.dli_sname || !info.dli_fname) {
    printf("DL-005 FAIL: dladdr ok but sname/fname not filled (sname=%p "
           "fname=%p)\n",
           (void *)info.dli_sname, (void *)info.dli_fname);
    rc = 1;
  } else {
    printf("DL-005 ok: dladdr → sname=\"%s\" fname=\"%s\" saddr=%p\n",
           info.dli_sname, info.dli_fname, info.dli_saddr);
  }

  // DL-006: dlclose on the head handle. __dl_invalid_handle walks the head
  // chain and finds head → returns 0 (valid); dlclose returns that 0.
  int cl = dlclose(handle);
  if (cl != 0) {
    printf("DL-006 FAIL: dlclose returned %d (expected 0)\n", cl);
    rc = 1;
  } else {
    printf("DL-006 ok: dlclose(head) = 0\n");
  }

  printf("test_dl: %s\n", rc == 0 ? "PASS" : "FAIL");
  return rc;
}

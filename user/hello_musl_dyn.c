/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * hello_musl_dyn — Phase 1.5 go/no-go probe (ldso.md).
 *
 * Minimal dynamic ELF linked against musl's fused libc.so (built by the musl
 * subproject, build/musl/lib/libc.so) with musl's own Scrt1/crti/crtn.
 * PT_INTERP is /lib/ld-musl-x86_64.so.1 (= musl libc.so shipped under that name
 * in the image). The musl loader self-bootstraps, sets fs_base via arch_prctl,
 * loads + relocates, jumps AT_ENTRY → musl _start → __libc_start_main → main.
 *
 * Compiled against musl's OWN public headers (not repo user/include) so the
 * compiled code matches musl's ABI.
 */

#include <stdio.h>

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  printf("hello, musl\n");
  return 0;
}

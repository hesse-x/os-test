/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * musl-side startup bridge — sets the __libc fields that musl's own
 * __libc_start_main would set but which our custom __libc_start_main
 * (user/lib/start_main.cc) cannot, because it deliberately does NOT pull in
 * musl's private struct __libc layout.
 *
 * Compiled into the musl_pthread sub-library (add_musl_lib), so it sees musl's
 * internal headers (src/internal/libc.h → struct __libc, libc macro).
 *
 * Why this exists: musl's pthread_create reads libc.page_size (via the
 * PAGE_SIZE macro = libc.page_size) and libc.tls_size. __init_tls sets the
 * TLS fields but NOT page_size; musl's __libc_start_main sets page_size from
 * aux[AT_PAGESZ]. We keep our own __libc_start_main (musl's reads AT_SYSINFO /
 * AT_UID / AT_SECURE / ... which our kernel does not provide), so this bridge
 * fills page_size + auxv for it.
 *
 * auxv pointer is also stored (pthread_getattr_np walks it to probe the main
 * stack via mremap). Harmless to set even if unused.
 */

#include "libc.h"
#include <elf.h>
#include <stddef.h>

// aux is the flat AT_*-indexed array (decoded from the kernel's pair-form auxv
// by start_main.cc's decode_auxv); page_size comes from aux[AT_PAGESZ].
// auxv_pairs is the original pair-form auxv ([type,value]*), stored as
// libc.auxv for pthread_getattr_np's mremap-based main-stack probe (musl's
// __init_libc does the same: libc.auxv = the pair-form pointer).
void musl_libc_init_aux(size_t *aux, size_t *auxv_pairs) {
  libc.auxv = auxv_pairs;
  libc.page_size = aux[AT_PAGESZ];
}

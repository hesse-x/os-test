/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// musl_loader_shim — the few symbols the musl dynamic loader (dynlink.lo/
// dlstart.lo, fused into libc.so per ldso.md) references that are NOT supplied
// by either the hand-written libc or the musl_pthread sub-library
// (src/thread/*, src/env/__init_tls.c, src/internal/libc.c, src/signal/block.c,
// ... merged into libc.so via $<TARGET_OBJECTS:musl_pthread>). Everything
// pthread/TLS-related
// (__libc, __hwcap, __init_tp, __copy_tls, __block_all_sigs/__restore_sigs,
// __inhibit_ptc/__release_ptc, __tls_get_addr) now comes from musl_pthread for
// real — the loader runs musl's actual TLS setup during __dls3 and the
// hand-written __libc_start_main no longer overrides fs_base. Only the
// loader-only / non-pthread symbols remain here.
//
// Symbol surface still provided here (loader refs not covered elsewhere):
//   __libc_get_version                    — ldd banner (dynlink.c:1554; never
//                                            on the normal exec path)
//   __tlsdesc_static, __tlsdesc_dynamic   — TLSDESC handlers whose addresses
//                                            are stored by R_X86_64_TLSDESC
//                                            relocs (dynlink.c:434/437). musl
//                                            defines them in ldso/tlsdesc.c,
//                                            which is NOT in musl_loader_objs
//                                            nor musl_pthread, so the shim
//                                            must provide them. They are only
//                                            reached under -mtls-dialect=desc,
//                                            which we never use.
//
// NOT here anymore (now supplied elsewhere, so dropped from the shim):
//   dprintf, vdprintf                     — musl src/stdio/{d,vd}printf.c
//                                            (musl_stdio_objs). Verified that
//                                            every loader call site runs AFTER
//                                            reloc_all(&ldso) (dynlink.c:1432),
//                                            i.e. with the loader's own PLT
//                                            already relocated, so the musl
//                                            native impl (routes through
//                                            vfprintf) is safe. The previous
//                                            boot-safe raw-syscall pair that
//                                            lived here has been removed.
//   __dl_vseterr / __dl_seterr / dlerror  — musl's dlerror.c (in musl_dl_objs)
//                                            provides them now that
//                                            musl_pthread gives the full struct
//                                            pthread (dlerror_buf/dlerror_flag)
//                                            at %fs:0.
//   __tls_get_addr                        — musl_pthread
//   (src/thread/__tls_get_addr.c).
//   getdelim                              — musl src/stdio/getdelim.c
//                                            (musl_stdio_objs, stdio.md). The
//                                            loader's /etc/ld.so.* read path is
//                                            unreachable on this OS (no such
//                                            files), so musl's version is a
//                                            drop-in.
//
// __environ is defined in start_main.cc (environ is aliased to it); declared
// extern here because the loader (dynlink.c:1464) writes it during __dls3.

#include <stddef.h>

extern char **__environ;

// ===================== ldd / version (never on exec path)
// =====================
const char *__libc_get_version(void) {
  return "1.1.19 (fused hand-written libc + musl pthread)";
}

// ===================== TLSDESC handlers (address taken by reloc)
// ===================== Only emitted with -mtls-dialect=desc; our objects never
// use TLSDESC, so the reloc branch that stores these addresses never runs and
// they are never called. musl's real definitions live in ldso/tlsdesc.c, which
// is not part of musl_loader_objs or musl_pthread, so the shim must satisfy the
// reference.
ptrdiff_t __tlsdesc_static(void) { return 0; }
ptrdiff_t __tlsdesc_dynamic(void) { return 0; }

// ===================== dprintf / vdprintf — supplied by musl stdio
// ===================== The loader resolves dprintf/vdprintf to musl's
// src/stdio/{d,vd}printf.c (musl_stdio_objs), which route through vfprintf.
// Every loader call site (dynlink.c error/ldd paths) runs AFTER
// reloc_all(&ldso) (dynlink.c:1432), so the loader's own PLT is already
// relocated by the time vfprintf's PLT is reached — the musl native impl is
// safe. A boot-safe raw-syscall pair used to live here as insurance; it was
// removed after verifying the ordering holds.

// ===================== getdelim — REMOVED (stdio.md) =====================
// musl's src/stdio/getdelim.c (now built via musl_stdio_objs) supplies the real
// getdelim; the hand-written fgetc-loop that used to live here would
// multi-define it. The loader reads /etc/ld.so.* via getdelim, but those files
// are absent on this OS (fopen returns NULL upstream), so the symbol is never
// reached on the loader path — musl's version is a drop-in either way.

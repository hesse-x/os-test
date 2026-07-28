/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// musl_loader_shim — symbols the musl dynamic loader (dynlink.lo/dlstart.lo,
// fused into libc.so per ldso.md) references but the hand-written libc does not
// define. We do NOT link musl's src/*.o (would clash with the hand-written
// libc), so these musl-internal symbols must be provided here.
//
// The loader is dormant after it jumps to the main ELF entry (ldso.md §2:
// "loader 跳 entry 后 dormant,覆盖安全"). The hand-written __libc_start_main
// then overrides fs_base to the hand-written struct tcb. Hence the TLS-model
// symbols (__init_tp/__copy_tls) only need to survive the brief bootstrap
// window; they are NOT a faithful musl pthread implementation. dlopen/real TLS
// is Phase 3+ (ldso.md §3 Phase 3 / §5.3).
//
// Symbol surface (from `nm -u` on dynlink.lo + dlstart.lo, minus what the
// hand-written libc already defines):
//   __libc, __hwcap, __environ            — musl globals (struct __libc must
//                                          match musl's libc.h field-for-field)
//   __init_tp, __copy_tls                 — TLS setup (shimmed; fs_base later
//                                          overridden by __libc_tls_init_rest)
//   __libc_get_version                    — ldd banner (never on exec path)
//   __block_all_sigs, __restore_sigs      — signal mask (only __tls_get_new)
//   __inhibit_ptc, __release_ptc          — lazy-TLS ptc lock (only dlopen/gd)
//   __tlsdesc_static, __tlsdesc_dynamic   — TLSDESC handlers (address taken by
//                                          reloc; never called w/o -mtlsdesc)
//   __tls_get_addr                        — general-dynamic TLS (no __thread →
//                                          never called)
//   __dl_vseterr                          — dlerror formatter (dlopen errors)
//   dprintf, vdprintf                     — loader debug/error messages
//                                          (real impl via vsnprintf+write)
//   getdelim                              — reading /etc/ld.so.* (absent on
//                                          this OS; real impl for safety)
//
// _dl_link_map: the hand-written ldso used to build this list; the fused
// loader does not. Defined here as NULL so the (legacy) dynamic-path
// collect_tls_from_link_map(_dl_link_map) returns empty tls_info without
// crashing — the new __libc_start_main dynamic path does not use it.

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <syscall.h>
#include <unistd.h>

#include <xos/errno.h>

// ===================== musl struct __libc (verbatim) =====================
// Copied field-for-field from third_party/musl/src/internal/libc.h so the
// loader's `libc.<field>` accesses hit the right offsets. Do NOT reorder.

struct __locale_struct {
  const struct __locale_map *volatile cat[6];
};
struct tls_module {
  struct tls_module *next;
  void *image;
  size_t len, size, align, offset;
};
struct __libc {
  int can_do_threads;
  int threaded;
  int secure;
  volatile int threads_minus_1;
  size_t *auxv;
  struct tls_module *tls_head;
  size_t tls_size, tls_align, tls_cnt;
  size_t page_size;
  struct __locale_struct global_locale;
};

// `__libc` is referenced HIDDEN by the loader. Define it here (zeroed BSS);
// the loader fills auxv/page_size/secure/tls_size during __dls3.
struct __libc __libc;

size_t __hwcap;
// __environ is defined in start_main.cc (where `environ` is aliased to it, so
// getenv reads the loader's __environ = envp assignment before __libc_env_init
// runs). Declared extern here; the loader (dynlink.c:1464) writes it.
extern char **__environ;

// ===================== TLS setup (bootstrap-only shims) =====================
// musl's real __copy_tls/__init_tp build a musl struct pthread at fs:0. We
// override fs_base to struct tcb in __libc_start_main, so these only need to
// (a) set fs_base to a valid pointer (so the loader's __pthread_self() inline
// %fs:0 read during __dls3 returns a comparable pointer), and
// (b) keep __dls3's `__copy_tls(builtin_tls) != __pthread_self()` assertion
//     true: __copy_tls returns its arg unchanged, and __init_tp sets fs:0 to
//     that same arg → the two compare equal.
//
// p points at musl's builtin_tls, laid out as struct pthread. musl and the
// hand-written tcb both put `self` at offset 0 and read the thread pointer via
// `mov %fs:0` (single dereference — see pthread_arch.h /
// __pthread_current_tcb).
// __init_tp MUST write self=p *before* setting fs_base, otherwise the very
// first errno-path syscall in the loader (e.g. writev → __syscall_ret →
// __errno_location, which does *(fs:0) to get the tcb) reads self=0 from BSS
// and returns &errno = 0 + 0x458 → SIGSEGV. Mirrors upstream __init_tls.c:14.
void *__copy_tls(unsigned char *mem) { return mem; }

int __init_tp(void *p) {
  struct pthread_shim {
    struct pthread_shim *self;
  };
  ((struct pthread_shim *)p)->self = (struct pthread_shim *)p;
  sys_arch_prctl(ARCH_SET_FS, (int64_t)(uintptr_t)p);
  __libc.can_do_threads = 1;
  return 0;
}

// ===================== ldd / version (never on exec path)
// =====================
const char *__libc_get_version(void) {
  return "1.1.19 (fused hand-written libc)";
}

// ===================== signal mask (only __tls_get_new / dlopen)
// ===================== Stubs: the exec bootstrap never calls them. Real impl
// belongs to Phase 3.
void __block_all_sigs(void *set) { (void)set; }
void __restore_sigs(void *set) { (void)set; }

// ===================== lazy-TLS / pthread-tp-cache lock (dlopen only)
// =====================
void __inhibit_ptc(void) {}
void __release_ptc(void) {}

// ===================== TLSDESC handlers (address taken by reloc)
// ===================== Only emitted with -mtls-dialect=desc; our objects never
// use TLSDESC, so the reloc branch that stores these addresses never runs and
// they are never called.
ptrdiff_t __tlsdesc_static(void) { return 0; }
ptrdiff_t __tlsdesc_dynamic(void) { return 0; }

// ===================== general-dynamic TLS (no __thread in hand-written libc)
// =====================
void *__tls_get_addr(size_t *v) {
  (void)v;
  return NULL;
}

// ===================== dlerror formatter (dlopen errors only)
// =====================
void __dl_vseterr(const char *fmt, va_list ap) {
  (void)fmt;
  (void)ap;
}

// ===================== dprintf / vdprintf (loader debug + error paths)
// ===================== The hand-written libc has snprintf/vsnprintf but not
// dprintf/vdprintf. The loader calls them for LD_DEBUG output and load-failure
// diagnostics; provide real implementations backed by vsnprintf + write.

int vdprintf(int fd, const char *fmt, va_list ap) {
  char buf[512];
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  if (n < 0)
    return -1;
  size_t w = (size_t)n;
  if (w > sizeof(buf))
    w = sizeof(buf);
  ssize_t r = write(fd, buf, w);
  return r < 0 ? -1 : n;
}

int dprintf(int fd, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vdprintf(fd, fmt, ap);
  va_end(ap);
  return n;
}

// ===================== getdelim (loader reads /etc/ld.so.* — absent here)
// ===================== Real implementation: the loader calls getdelim on an
// already-opened FILE* to read library-path config. On this OS those files do
// not exist so fopen returns NULL upstream and getdelim is never reached;
// implement it properly regardless so a future config file works.
ssize_t getdelim(char **lineptr, size_t *n, int delim, FILE *stream) {
  if (!lineptr || !n || !stream) {
    errno = EINVAL;
    return -1;
  }
  size_t i = 0;
  int c;
  // Ensure a minimum buffer.
  if (!*lineptr || *n == 0) {
    *n = 128;
    *lineptr = (char *)malloc(*n);
    if (!*lineptr) {
      errno = ENOMEM;
      return -1;
    }
  }
  while ((c = fgetc(stream)) != EOF) {
    if (i + 1 >= *n) {
      size_t newcap = *n * 2;
      char *p = (char *)realloc(*lineptr, newcap);
      if (!p) {
        errno = ENOMEM;
        return -1;
      }
      *lineptr = p;
      *n = newcap;
    }
    (*lineptr)[i++] = (char)c;
    if (c == delim)
      break;
  }
  if (i == 0)
    return -1; // EOF, nothing read
  (*lineptr)[i] = '\0';
  return (ssize_t)i;
}

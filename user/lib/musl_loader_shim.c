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
//   dprintf, vdprintf                     — loader debug/error messages
//                                            (real impl via vsnprintf+write)
//   getdelim                              — reading /etc/ld.so.* (absent on
//                                            this OS; real impl for safety)
//
// NOT here anymore (now supplied elsewhere, so dropped from the shim):
//   __dl_vseterr / __dl_seterr / dlerror  — musl's dlerror.c (in musl_dl_objs)
//                                            provides them now that
//                                            musl_pthread gives the full struct
//                                            pthread (dlerror_buf/dlerror_flag)
//                                            at %fs:0.
//   __tls_get_addr                        — musl_pthread
//   (src/thread/__tls_get_addr.c).
//
// __environ is defined in start_main.cc (environ is aliased to it); declared
// extern here because the loader (dynlink.c:1464) writes it during __dls3.

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <xos/errno.h>
#include <xos/syscall_nums.h>

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

// ===================== dprintf / vdprintf (loader debug + error paths)
// ===================== ld.so may diagnose failures before relocating its own
// PLT. Keep this tiny formatter self-contained and write through raw syscalls
// only.

static long loader_raw_write(int fd, const char *buf, size_t len) {
  long ret;
  __asm__ volatile("syscall"
                   : "=a"(ret)
                   : "a"((long)SYS_WRITE), "D"((long)fd), "S"(buf),
                     "d"((long)len)
                   : "rcx", "r11", "memory");
  return ret;
}

static int loader_write_str(int fd, const char *s) {
  size_t len = 0;
  if (!s)
    s = "(null)";
  while (s[len])
    len++;
  long ret = loader_raw_write(fd, s, len);
  return ret < 0 ? -1 : (int)ret;
}

static int loader_write_uint(int fd, uint64_t value, unsigned base) {
  static const char digits[] = "0123456789abcdef";
  char buf[32];
  size_t pos = sizeof(buf);
  do {
    buf[--pos] = digits[value % base];
    value /= base;
  } while (value);
  long ret = loader_raw_write(fd, buf + pos, sizeof(buf) - pos);
  return ret < 0 ? -1 : (int)ret;
}

static int loader_vprint(int fd, const char *fmt, va_list ap) {
  int total = 0;
  const char *text = fmt;

  while (*fmt) {
    if (*fmt++ != "%"[0])
      continue;
    if (fmt - 1 > text) {
      long ret = loader_raw_write(fd, text, (size_t)((fmt - 1) - text));
      if (ret < 0)
        return -1;
      total += (int)ret;
    }

    int ret;
    if (*fmt == "s"[0]) {
      ret = loader_write_str(fd, va_arg(ap, const char *));
      fmt++;
    } else if (*fmt == "d"[0]) {
      int value = va_arg(ap, int);
      if (value < 0) {
        ret = (int)loader_raw_write(fd, "-", 1);
        if (ret >= 0) {
          int n = loader_write_uint(fd, 0u - (unsigned)value, 10);
          ret = n < 0 ? -1 : ret + n;
        }
      } else {
        ret = loader_write_uint(fd, (unsigned)value, 10);
      }
      fmt++;
    } else if (*fmt == "z"[0] && fmt[1] == "u"[0]) {
      ret = loader_write_uint(fd, va_arg(ap, size_t), 10);
      fmt += 2;
    } else if (*fmt == "p"[0]) {
      ret = (int)loader_raw_write(fd, "0x", 2);
      if (ret >= 0) {
        int n = loader_write_uint(fd, (uintptr_t)va_arg(ap, void *), 16);
        ret = n < 0 ? -1 : ret + n;
      }
      fmt++;
    } else if (*fmt == "m"[0]) {
      ret = loader_write_str(fd, "<errno>");
      fmt++;
    } else if (*fmt == "%"[0]) {
      ret = (int)loader_raw_write(fd, "%", 1);
      fmt++;
    } else {
      ret = (int)loader_raw_write(fd, "%", 1);
    }
    if (ret < 0)
      return -1;
    total += ret;
    text = fmt;
  }

  if (fmt > text) {
    long ret = loader_raw_write(fd, text, (size_t)(fmt - text));
    if (ret < 0)
      return -1;
    total += (int)ret;
  }
  return total;
}

int vdprintf(int fd, const char *fmt, va_list ap) {
  return loader_vprint(fd, fmt, ap);
}

int dprintf(int fd, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int ret = loader_vprint(fd, fmt, ap);
  va_end(ap);
  return ret;
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

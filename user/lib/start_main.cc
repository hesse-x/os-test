/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * Process startup / lifecycle:
 *   __libc_start_main, atexit, .init_array/.fini_array, environ
 *
 * Merged from start_main.cc + atexit.c + init_array.c + environ.c
 */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/cdefs.h>
#include <syscall.h>
#include <xos/elf.h>
#include <xos/errno.h>

// ===================== atexit =====================
// Static array of up to 32 handlers (no malloc dependency)

#define ATEXIT_MAX 32

typedef void (*atexit_func_fn)(void);

static atexit_func_fn atexit_funcs[ATEXIT_MAX];
static int atexit_count = 0;

extern "C" int atexit(void (*func)(void)) {
  if (atexit_count >= ATEXIT_MAX)
    return -1;
  atexit_funcs[atexit_count++] = func;
  return 0;
}

extern "C" void __libc_run_atexit(void) {
  for (int i = atexit_count - 1; i >= 0; i--) {
    if (atexit_funcs[i])
      atexit_funcs[i]();
  }
}

// ===================== .init_array / .fini_array =====================

typedef void (*init_func_fn)(void);

extern "C" void __libc_run_init_array(init_func_fn *start, init_func_fn *end) {
  for (init_func_fn *f = start; f < end; f++) {
    if (*f)
      (*f)();
  }
}

extern "C" void __libc_run_fini_array(init_func_fn *start, init_func_fn *end) {
  // Empty range (incl. the dynamic path's nullptr/nullptr, where the loader
  // owns fini ordering via its own __libc_exit_fini): `end - 1` would underflow
  // to (uintptr_t)-8 and the `f >= start` guard is an unsigned compare, so the
  // loop body would deref -8 → SIGSEGV at exit. Bail out on an empty range.
  if (!end || end <= start)
    return;
  for (init_func_fn *f = end - 1; f >= start; f--) {
    if (*f)
      (*f)();
  }
}

// ===================== environ =====================
// environ is a malloc-growable char** array (NULL-terminated).
//
// environ and __environ MUST be the same pointer. The musl loader sets
// __environ = envp (the on-stack environment) during __dls3, then calls
// getenv("LD_LIBRARY_PATH")/getenv("LD_PRELOAD") BEFORE __libc_start_main runs
// __libc_env_init. Our getenv/env_find read `environ`, so if `environ` were a
// separate global (NULL until __libc_env_init) the loader's getenv would return
// NULL and LD_LIBRARY_PATH would be silently dropped — liba.so/libb.so in
// /test/lib would not be found (ld_chain/diamond/cycle exit 127). Aliasing
// environ to __environ (mirroring musl's weak_alias(__environ, environ)) makes
// the loader's assignment immediately visible to getenv. __libc_env_init later
// reallocs the shared pointer into a heap-backed growable copy.
extern "C" char **__environ = 0;
extern "C" char **environ __attribute__((alias("__environ")));

static pthread_mutex_t env_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t env_capacity = 0;

static int env_reserve(size_t count) {
  size_t need = count + 1;
  if (env_capacity >= need)
    return 0;
  size_t newcap = env_capacity ? env_capacity : 8;
  while (newcap < need)
    newcap *= 2;
  char **p = (char **)realloc(environ, newcap * sizeof(char *));
  if (!p)
    return -1;
  environ = p;
  env_capacity = newcap;
  return 0;
}

static size_t env_count(void) {
  size_t n = 0;
  if (environ)
    while (environ[n])
      n++;
  return n;
}

static int env_find(const char *name, char **out_val) {
  if (!environ)
    return -1;
  size_t namelen = strlen(name);
  for (size_t i = 0; environ[i]; i++) {
    if (strncmp(environ[i], name, namelen) == 0 && environ[i][namelen] == '=') {
      if (out_val)
        *out_val = environ[i] + namelen + 1;
      return (int)i;
    }
  }
  return -1;
}

extern "C" char *getenv(const char *name) {
  pthread_mutex_lock(&env_lock);
  char *val = NULL;
  int idx = env_find(name, &val);
  (void)idx;
  pthread_mutex_unlock(&env_lock);
  return val;
}

extern "C" int setenv(const char *name, const char *value, int overwrite) {
  if (!name || !*name || strchr(name, '=')) {
    errno = EINVAL;
    return -1;
  }
  size_t namelen = strlen(name);
  size_t valuelen = value ? strlen(value) : 0;
  char *entry = (char *)malloc(namelen + 1 + valuelen + 1);
  if (!entry) {
    errno = ENOMEM;
    return -1;
  }
  memcpy(entry, name, namelen);
  entry[namelen] = '=';
  if (value)
    memcpy(entry + namelen + 1, value, valuelen + 1);
  else
    entry[namelen + 1] = '\0';

  pthread_mutex_lock(&env_lock);
  int idx = env_find(name, NULL);
  if (idx >= 0) {
    if (!overwrite) {
      free(entry);
      pthread_mutex_unlock(&env_lock);
      return 0;
    }
    environ[idx] = entry;
    pthread_mutex_unlock(&env_lock);
    return 0;
  }
  size_t n = env_count();
  if (env_reserve(n + 1) < 0) {
    free(entry);
    errno = ENOMEM;
    pthread_mutex_unlock(&env_lock);
    return -1;
  }
  environ[n] = entry;
  environ[n + 1] = NULL;
  pthread_mutex_unlock(&env_lock);
  return 0;
}

extern "C" int putenv(char *string) {
  if (!string) {
    errno = EINVAL;
    return -1;
  }
  char *eq = strchr(string, '=');
  if (!eq) {
    errno = EINVAL;
    return -1;
  }
  size_t namelen = (size_t)(eq - string);

  pthread_mutex_lock(&env_lock);
  for (size_t i = 0; environ && environ[i]; i++) {
    if (strncmp(environ[i], string, namelen) == 0 &&
        environ[i][namelen] == '=') {
      environ[i] = string;
      pthread_mutex_unlock(&env_lock);
      return 0;
    }
  }
  size_t n = env_count();
  if (env_reserve(n + 1) < 0) {
    errno = ENOMEM;
    pthread_mutex_unlock(&env_lock);
    return -1;
  }
  environ[n] = string;
  environ[n + 1] = NULL;
  pthread_mutex_unlock(&env_lock);
  return 0;
}

extern "C" int unsetenv(const char *name) {
  if (!name || !*name || strchr(name, '=')) {
    errno = EINVAL;
    return -1;
  }
  pthread_mutex_lock(&env_lock);
  int idx = env_find(name, NULL);
  if (idx < 0) {
    pthread_mutex_unlock(&env_lock);
    return 0;
  }
  size_t n = env_count();
  for (size_t i = (size_t)idx; i < n; i++)
    environ[i] = environ[i + 1];
  pthread_mutex_unlock(&env_lock);
  return 0;
}

extern "C" int clearenv(void) {
  pthread_mutex_lock(&env_lock);
  free(environ);
  environ = NULL;
  env_capacity = 0;
  pthread_mutex_unlock(&env_lock);
  return 0;
}

extern "C" void __libc_env_init(char **envp) {
  pthread_mutex_lock(&env_lock);
  if (!envp) {
    environ = (char **)malloc(sizeof(char *));
    if (environ) {
      environ[0] = NULL;
      env_capacity = 1;
    }
    pthread_mutex_unlock(&env_lock);
    return;
  }
  size_t n = 0;
  while (envp[n])
    n++;
  if (env_reserve(n) < 0) {
    pthread_mutex_unlock(&env_lock);
    return;
  }
  for (size_t i = 0; i < n; i++)
    environ[i] = envp[i];
  environ[n] = NULL;
  pthread_mutex_unlock(&env_lock);
}

// ===================== __libc_start_main =====================
// musl's 6-arg dynamic ABI: called by musl Scrt1/crt1 _start_c as
//   __libc_start_main(main, argc, argv, _init, _fini, 0)
// (crt/crt1.c). _init/_fini are the crti/crtn .init/.fini brackets; the 6th
// arg is the loader's ldso_fini (NULL here — we exit via sys_exit_group).
//
// TLS / thread-pointer init delegates to musl's __init_tls (compiled into the
// musl_pthread sub-library, merged into libc.a + libc.so). musl's __init_tls
// scans the auxv for PT_TLS (via AT_PHDR/AT_PHNUM/AT_PHENT), mmaps the TLS
// block, and sets FS_BASE via __init_tp (arch_prctl SET_FS), set_tid_address,
// and the robust-list head — all the per-thread state musl pthread_create
// expects. errno lives at %fs:0 (musl TCB), so every ELF must run __init_tls
// at startup or the first errno fetch crashes.
//
// Static path (DYNAMIC=0): no loader — __libc_start_main decodes the kernel-
// built pair-form auxv into the flat AT_*-indexed array musl expects, then
// calls __init_tls + __init_ssp + musl_libc_init_aux (sets libc.page_size /
// libc.auxv, which __init_tls does not set but pthread_create reads). TLS
// template comes from a PT_TLS segment the linker emits; .init_array ranges
// come from user_linker.ld symbols (__init_array_start/_end).
//
// Dynamic path (DYNAMIC=1): the musl loader (fused into libc.so) already ran
// __dls3, which decoded the auxv, set __environ / libc.page_size / libc.secure
// / __hwcap, and called __init_tp(__copy_tls(builtin_tls)) to set FS_BASE to a
// musl struct pthread. We therefore do NOT re-init TLS here — only run the
// loader's __libc_start_init() (do_init_fini(tail)) to fire .init_array of
// every loaded DSO (incl. the main ELF).

extern "C" void __init_tls(size_t *aux);
extern "C" void __init_ssp(void *entropy);
extern "C" void musl_libc_init_aux(size_t *aux, size_t *auxv_pairs);
extern "C" void
__libc_start_init(void); // musl loader (dynlink.c): do_init_fni(tail)

#if !DYNAMIC
// user_linker.ld symbols (static main ELF only — Scrt1/crt1 do not pass
// init_array ranges to __libc_start_main).
extern "C" init_func_fn __init_array_start[];
extern "C" init_func_fn __init_array_end[];
extern "C" init_func_fn __fini_array_start[];
extern "C" init_func_fn __fini_array_end[];
#endif

static init_func_fn *g_fini_start;
static init_func_fn *g_fini_end;

extern "C" void __libc_fini_array_trampoline(void) {
  __libc_run_fini_array(g_fini_start, g_fini_end);
}

#if !DYNAMIC
// Walk envp to its terminating NULL, returning a pointer to the auxv that
// follows it on the kernel-built stack
// ([argc][argv…][NULL][envp…][NULL][auxv…]).
static size_t *auxv_from_envp(char **envp) {
  char **p = envp;
  while (*p)
    p++;
  return (size_t *)(p + 1);
}

// musl's __init_tls / __init_ssp index aux as a FLAT array keyed by AT_* value
// (aux[AT_PHDR], aux[AT_RANDOM], ...), NOT as the kernel-built [type,value]*
// pairs. musl's own __libc_start_main decodes the pair-form auxv into this flat
// form before calling __init_tls (src/env/__libc_start_main.c:28). Our custom
// __libc_start_main must do the same decode on the static path, or __init_tls
// reads aux[AT_PHDR] (= aux[3]) as the 4th element of the pair-form auxv
// (AT_PHENT's value 0x38) and crashes dereferencing it as a Phdr pointer.
// AUX_CNT mirrors musl (38). The dynamic path does not need this: the loader's
// __dls3 already decoded the auxv and ran __init_tls itself.
#define AUX_CNT 38
static void decode_auxv(size_t *auxv_pairs, size_t aux[AUX_CNT]) {
  for (size_t i = 0; i < AUX_CNT; i++)
    aux[i] = 0;
  for (size_t i = 0; auxv_pairs[i]; i += 2)
    if (auxv_pairs[i] < AUX_CNT)
      aux[auxv_pairs[i]] = auxv_pairs[i + 1];
}
#endif

extern "C" LIBC_EXPORT int __libc_start_main(int (*main)(int, char **, char **),
                                             int argc, char **argv,
                                             void (*init)(void),
                                             void (*fini)(void),
                                             void (*ldso_fini)(void)) {
  (void)init;
  (void)fini;
  (void)ldso_fini;

  char **envp = argv + argc + 1;

#if DYNAMIC
  // The fused musl loader already set up TLS (FS_BASE → musl struct pthread),
  // __environ, and libc.{auxv,page_size,secure} during __dls3. Only run the
  // .init_array of every loaded DSO (incl. the main ELF) via the loader's
  // do_init_fni(tail). No fini-array range to trampoline (loader owns DSO
  // fini via atexit-registered __libc_start_init).
  __libc_start_init();
  g_fini_start = nullptr;
  g_fini_end = nullptr;
#else
  // Static path: no loader. Decode the kernel pair-form auxv and run musl's
  // __init_tls (mmaps PT_TLS, sets FS_BASE via __init_tp, set_tid_address,
  // robust-list head). __init_tls must run before __init_ssp: __init_ssp
  // writes the stack canary via __pthread_self() (mov %fs:0), valid only after
  // __init_tp sets FS_BASE. musl_libc_init_aux sets libc.page_size (read by
  // pthread_create's ROUND macro) and libc.auxv (walked by pthread_getattr_np);
  // __init_tls sets neither. Then fire the main ELF's .init_array.
  size_t *auxv_pairs = auxv_from_envp(envp);
  size_t aux[AUX_CNT];
  decode_auxv(auxv_pairs, aux);
  __init_tls(aux);
  musl_libc_init_aux(aux, auxv_pairs);
  __init_ssp((void *)aux[AT_RANDOM]);
  __libc_run_init_array(__init_array_start, __init_array_end);
  g_fini_start = __fini_array_start;
  g_fini_end = __fini_array_end;
#endif
  atexit(__libc_fini_array_trampoline);

  __libc_env_init(envp);

  int ret = main(argc, argv, envp);

  fflush(stdout);
  __libc_run_atexit();
  sys_exit_group(ret);
  __builtin_unreachable();
}

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
#include <sys/tls.h>
#include <syscall.h>
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
  for (init_func_fn *f = end - 1; f >= start; f--) {
    if (*f)
      (*f)();
  }
}

// ===================== environ =====================
// environ is a malloc-growable char** array (NULL-terminated).

extern "C" {
char **environ = NULL;
}

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
// Unified startup entry, single source dual products

extern "C" void __libc_tls_init(void);
extern "C" void __libc_tls_init_rest(void);
extern "C" struct tls_info g_tls_info;

#if DYNAMIC
#include <sys/link_map.h>
extern "C" struct tls_info collect_tls_from_link_map(struct link_map *lmap);
#endif

static init_func_fn *g_fini_start;
static init_func_fn *g_fini_end;

extern "C" void __libc_fini_array_trampoline(void) {
  __libc_run_fini_array(g_fini_start, g_fini_end);
}

extern "C" LIBC_EXPORT int
__libc_start_main(int (*main)(int, char **, char **), int argc, char **argv,
                  init_func_fn *init_start, init_func_fn *init_end,
                  init_func_fn *fini_start, init_func_fn *fini_end) {
#if DYNAMIC
  g_tls_info = collect_tls_from_link_map(_dl_link_map);
  __libc_tls_init_rest();
#else
  __libc_tls_init();
#endif

  __libc_run_init_array(init_start, init_end);

  g_fini_start = fini_start;
  g_fini_end = fini_end;
  atexit(__libc_fini_array_trampoline);

  char **envp = argv + argc + 1;
  __libc_env_init(envp);

  int ret = main(argc, argv, envp);

  fflush(stdout);
  __libc_run_atexit();
  sys_exit_group(ret);
  __builtin_unreachable();
}

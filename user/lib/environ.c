/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* environ.c — Environment variables (D9 model)
 *
 * environ is a malloc-growable char** array (NULL-terminated).
 * __libc_start_main copies the on-stack envp into this array.
 * getenv/setenv/putenv/unsetenv/clearenv use an internal pthread_mutex to
 * protect all mutating operations.
 *
 * setenv: strdup("name=val") into the array, realloc to grow.
 * putenv: places the passed-in char* into the array without copying (glibc
 * semantics). execve callers pass environ by default (see sys_process.cc).
 */
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <xos/errno.h>

char **environ = NULL;

static pthread_mutex_t env_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t env_capacity =
    0; /* number of array slots (including NULL terminator) */

/* Ensure the array can hold at least count env strings + 1 NULL terminator */
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

/* Count the current number of entries in environ (excluding the NULL
 * terminator) */
static size_t env_count(void) {
  size_t n = 0;
  if (environ)
    while (environ[n])
      n++;
  return n;
}

/* Find the index of the name= entry; returns index, *out_val points after '='.
 * name does not contain '='. Returns -1 if not found. */
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

char *getenv(const char *name) {
  pthread_mutex_lock(&env_lock);
  char *val = NULL;
  int idx = env_find(name, &val);
  (void)idx;
  pthread_mutex_unlock(&env_lock);
  return val;
}

int setenv(const char *name, const char *value, int overwrite) {
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

int putenv(char *string) {
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
      environ[i] = string; /* no copy, glibc semantics */
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

int unsetenv(const char *name) {
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
  /* Shift-remove to keep the array compact */
  size_t n = env_count();
  for (size_t i = (size_t)idx; i < n; i++)
    environ[i] = environ[i + 1];
  pthread_mutex_unlock(&env_lock);
  return 0;
}

int clearenv(void) {
  pthread_mutex_lock(&env_lock);
  free(environ);
  environ = NULL;
  env_capacity = 0;
  pthread_mutex_unlock(&env_lock);
  return 0;
}

/* Called by __libc_start_main: copy the on-stack envp into a malloc'd char**
 * array. envp is a NULL-terminated array of strings. */
void __libc_env_init(char **envp) {
  pthread_mutex_lock(&env_lock);
  if (!envp) {
    /* No environment: allocate an empty array containing only the NULL
     * terminator */
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
    return; /* environ stays NULL, getenv returns NULL */
  }
  for (size_t i = 0; i < n; i++)
    environ[i] = envp[i];
  environ[n] = NULL;
  pthread_mutex_unlock(&env_lock);
}

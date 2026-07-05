/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* environ.c — 环境变量（D9 模型）
 *
 * environ 是 malloc 可增长的 char** 数组（NULL 终止）。__libc_start_main
 * 把栈上的 envp 拷成这个数组。getenv/setenv/putenv/unsetenv/clearenv 用
 * 一个内部 pthread_mutex 保护所有可变操作。
 *
 * setenv: strdup("name=val") 进数组，realloc 扩容。
 * putenv: 把传入 char*（不拷贝，glibc 语义）放进数组。
 * execve 调用方默认传 environ（见 sys_process.cc）。
 */
#include "xos/errno.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

char **environ = NULL;

static pthread_mutex_t env_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t env_capacity = 0; /* 数组槽位数（含 NULL 终止） */

/* 确保数组至少能容纳 count 个 env 字符串 + 1 个 NULL 终止 */
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

/* 计算 environ 当前条目数（不含 NULL 终止） */
static size_t env_count(void) {
  size_t n = 0;
  if (environ)
    while (environ[n])
      n++;
  return n;
}

/* 查找 name= 条目的下标，返回 index；*out_val 指向 '=' 之后。
 * name 不含 '='。未找到返回 -1。 */
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
      environ[i] = string; /* 不拷贝，glibc 语义 */
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
  /* 移位删除，保持紧凑 */
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

/* __libc_start_main 调用：把栈 envp 拷成 malloc 的 char** 数组。
 * envp 是 NULL 终止的字符串数组。 */
void __libc_env_init(char **envp) {
  pthread_mutex_lock(&env_lock);
  if (!envp) {
    /* 无环境：分配仅含 NULL 终止的空数组 */
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
    return; /* environ 保持 NULL，getenv 返回 NULL */
  }
  for (size_t i = 0; i < n; i++)
    environ[i] = envp[i];
  environ[n] = NULL;
  pthread_mutex_unlock(&env_lock);
}

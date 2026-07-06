/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>

// atexit implementation: a static array of up to 32 handlers (no malloc dependency)
// ld.md §6.4 task 5 / plan_ld2b3 T8

#define ATEXIT_MAX 32

typedef void (*atexit_func_t)(void);

static atexit_func_t atexit_funcs[ATEXIT_MAX];
static int atexit_count = 0;

int atexit(void (*func)(void)) {
  if (atexit_count >= ATEXIT_MAX)
    return -1;
  atexit_funcs[atexit_count++] = func;
  return 0;
}

// __libc_run_atexit: on exit, call all atexit handlers in reverse order
void __libc_run_atexit(void) {
  for (int i = atexit_count - 1; i >= 0; i--) {
    if (atexit_funcs[i])
      atexit_funcs[i]();
  }
}

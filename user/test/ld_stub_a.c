/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// ld.so multi-dependency unit-test stub: liba.so
// Exports lda_answer(); internally calls libc strcmp (triggers a cross-module
// JUMP_SLOT resolution)
#include <stddef.h>

int strcmp(const char *a, const char *b); // libc.so symbol

int lda_answer(void) {
  // Call a libc symbol to verify JUMP_SLOT resolves to libc.so
  if (strcmp("ld", "ld") != 0)
    return -1;
  return 41; // linear-chain scenario: main ELF expects 41
}

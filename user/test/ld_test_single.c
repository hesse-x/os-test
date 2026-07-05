/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// Scenario 1: single-dependency regression (NEEDED=libc.so)
// Verify that recursive loading degenerates to a no-op for a single
// dependency, leaving hello_dyn's current behavior intact
#include <stdio.h>
int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  if (printf("ld_single: ok\n") < 0)
    return 1; // libc.so JUMP_SLOT
  return 0;
}

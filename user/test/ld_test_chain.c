/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// Scenario 2: linear chain (main -> b -> {a, libc})
// Verify: load order has libc before b; ldb_chain/ldb_via_a resolve correctly
int ldb_chain(void);
int ldb_via_a(void);
int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  if (ldb_chain() != 42)
    return 1; // b->libc JUMP_SLOT
  if (ldb_via_a() != 42)
    return 1; // b->a->libc cross-module
  return 0;
}

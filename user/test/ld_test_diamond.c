/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// Scenario 3: diamond dedup (main -> {a, libc}, a -> libc)
// Verify: libc.so is loaded only once (dedup); lda_answer calls strcmp which
// resolves to the unique libc.so
int lda_answer(void);
int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  if (lda_answer() != 41)
    return 1; // a->libc JUMP_SLOT; after dedup libc is unique
  return 0;
}

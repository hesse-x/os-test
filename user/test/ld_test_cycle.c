/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// Scenario 4: cycle detection (a<->b mutual NEEDED)
// Expected: find_loaded dedup silently breaks the cycle; the process exits
// normally with no dl: FATAL
int lda_answer(void);
int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  if (lda_answer() != 41)
    return 1; // a is still usable after the cycle is broken
  return 0;
}

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// ld.so multi-dependency unit-test stub: libb.so
// Exports ldb_chain() (used by the linear-chain scenario) and ldb_via_a()
// (calls a liba symbol to verify b->a resolution)
int lda_answer(void); // liba.so symbol (JUMP_SLOT)

int ldb_chain(void) {
  return 42; // linear-chain scenario: main ELF expects 42
}

int ldb_via_a(void) {
  return lda_answer() + 1; // call liba to verify b->a cross-module JUMP_SLOT
}

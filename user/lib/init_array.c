/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// .init_array / .fini_array iteration
// ld.md §3.5.2, §6.4 task 4 / plan_ld2b3 T10
//
// Under dynamic linking __init_array_start/end are hidden symbols (not in .dynsym),
// so libc.so cannot reference them across objects. Instead crt0 takes the main ELF's
// symbol addresses and passes the range via __libc_start_main parameters.

typedef void (*init_func_t)(void);

// Run .init_array (C++ global constructors, etc.)
void __libc_run_init_array(init_func_t *start, init_func_t *end) {
  for (init_func_t *f = start; f < end; f++) {
    if (*f)
      (*f)();
  }
}

// Run .fini_array (in reverse order on exit)
void __libc_run_fini_array(init_func_t *start, init_func_t *end) {
  for (init_func_t *f = end - 1; f >= start; f--) {
    if (*f)
      (*f)();
  }
}

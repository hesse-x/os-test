/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// user/lib/start_main.cc — __libc_start_main unified startup (single source, dual products)
// ld.md §3.5.2 / plan_ld2b3 T11
//
// libc.a is compiled with -DDYNAMIC=0 (static path calls __libc_tls_init)
// libc.so is compiled with -DDYNAMIC=1 (dynamic path calls collect_tls_from_link_map +
// __libc_tls_init_rest)

#include <stdio.h>
#include <stdlib.h>
#include <sys/cdefs.h>
#include <sys/tls.h>
#include <syscall.h>

typedef void (*init_func_t)(void);
extern "C" void __libc_run_init_array(init_func_t *start, init_func_t *end);
extern "C" void __libc_run_fini_array(init_func_t *start, init_func_t *end);
extern "C" void __libc_run_atexit(void);
extern "C" void
__libc_env_init(char **envp); /* environ.c: copy stack envp → environ */

// Static-path TLS initialization (tls.cc): read linker symbols to fill g_tls_info +
// alloc TCB + FS_BASE + ...
extern "C" void __libc_tls_init(void);
extern "C" void __libc_tls_init_rest(
    void); // alloc TCB + FS_BASE + set_tid_address + cancel handler
extern "C" struct tls_info g_tls_info;

// Dynamic path: link_map list exported by ld.so
#if DYNAMIC
#include <sys/link_map.h>
extern "C" struct tls_info collect_tls_from_link_map(struct link_map *lmap);
#endif

// atexit callbacks take no arguments; use static variables to record the fini range
static init_func_t *g_fini_start;
static init_func_t *g_fini_end;
extern "C" void __libc_fini_array_trampoline(void) {
  __libc_run_fini_array(g_fini_start, g_fini_end);
}

// Unified startup function, single source dual products
// Parameters: main, argc, argv, init_array range [init_start, init_end),
//             fini_array range [fini_start, fini_end)
// (The original SysV ABI's init/fini/rtld_fini/stack_end are no longer used; the
// registers are repurposed to pass the ranges)
// De facto ABI: crt0.S calls across .so boundaries, so it must be exported
// (explicitly default under -fvisibility=hidden)
extern "C" LIBC_EXPORT int
__libc_start_main(int (*main)(int, char **, char **), int argc, char **argv,
                  init_func_t *init_start, init_func_t *init_end,
                  init_func_t *fini_start, init_func_t *fini_end) {
  // 1. TLS template discovery + main-thread TCB allocation (static/dynamic split)
#if DYNAMIC
  // Dynamic: walk _dl_link_map merging PT_TLS into g_tls_info, then alloc TCB
  g_tls_info = collect_tls_from_link_map(_dl_link_map);
  __libc_tls_init_rest();
#else
  // Static: __libc_tls_init does it all (first fills g_tls_info + rest allocs TCB + ...)
  __libc_tls_init();
#endif

  // 2. Run .init_array
  __libc_run_init_array(init_start, init_end);

  // 3. Register .fini_array with atexit
  g_fini_start = fini_start;
  g_fini_end = fini_end;
  atexit(__libc_fini_array_trampoline);
  // rtld_fini (ld.so's fini): in this design ld.so does not register it, pass NULL

  // 4. Compute envp
  char **envp = argv + argc + 1;

  // 4b. Initialize environ (copy stack envp into a malloc'd char** array)
  __libc_env_init(envp);

  // 5. Run main
  int ret = main(argc, argv, envp);

  // 6. exit → run atexit handlers (including .fini_array) →
  // sys_exit_group (kill all threads)
  fflush(stdout);
  __libc_run_atexit();
  sys_exit_group(ret);
  __builtin_unreachable();
}

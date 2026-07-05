/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef USER_SYS_LINK_MAP_H
#define USER_SYS_LINK_MAP_H

#include <stddef.h>
#include <stdint.h>
#include <sys/cdefs.h>
#include <xos/elf.h>

#ifdef __cplusplus
extern "C" {
#endif

// link_map: loaded object descriptor built by ld.so
// libc.so reads this struct via _dl_link_map (used by collect_tls_from_link_map)
// ld.so's link_map.c defines the same struct; both sides must match
// plan_ld2b3 T12 / ld.md §3.3.7

struct link_map {
  uintptr_t base; // load base address
  char soname[64]; // DT_NEEDED soname (e.g. "libc.so"); may be empty for main ELF/ld.so
  void *dynamic;           // .dynamic segment pointer
  struct link_map *l_next; // next list entry
  struct link_map *l_prev; // previous list entry
  void *symtab;            // .symtab
  const char *strtab;      // .strtab
  void *gnu_hash;          // .gnu.hash
  void *rela_dyn;          // .rela.dyn
  size_t rela_dyn_sz;      // .rela.dyn size
  void *rela_plt;          // .rela.plt
  size_t rela_plt_sz;      // .rela.plt size
  void *tls_template;      // TLS template (.tdata start)
  size_t tls_tdata_size;   // .tdata size
  size_t tls_tbss_size;    // .tbss size
  size_t tls_align;        // TLS alignment
};

// Global link_map list head exported by ld.so
// visibility("default") matches ld.so's definition (ld.so is globally
// -fvisibility=hidden; only this symbol is exported). libc.so references it
// via GLOB_DAT relocation (already handled by ld.so bootstrap)
__attribute__((visibility("default"))) extern struct link_map *_dl_link_map;

// For libc.so: walk _dl_link_map merging PT_TLS into tls_info
struct tls_info;
LIBC_EXPORT extern struct tls_info
collect_tls_from_link_map(struct link_map *lmap);

// ld.so internal: find PT_TLS in PHDR and fill link_map's tls_* fields
// hidden visibility: avoid ld.so cross-module call going through PLT (before
// bootstrap GOT is unfilled, PLT jump lands on lazy stub 0x1016 -> #PF)
__attribute__((visibility("hidden"))) void
fill_tls_from_phdr(struct link_map *l, uintptr_t base, uintptr_t phdr,
                   size_t phent, size_t phnum);

// ld.so internal: parse .dynamic to fill link_map symbol-lookup fields
// (called by dls_init.c while dynamically building the list)
__attribute__((visibility("hidden"))) void
fill_link_map(struct link_map *l, uintptr_t base, Elf64_Dyn *dyn);

// ld.so's own static node (defined in link_map.c; dls_init.c takes its address
// and links it at the tail of the list)
__attribute__((visibility("hidden"))) extern struct link_map g_ld_map_static;

#ifdef __cplusplus
}
#endif

#endif // USER_SYS_LINK_MAP_H

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// ld.so link_map construction: ld.so's own static node + dynamically allocated
// nodes for main ELF / each .so
// ld.md §3.3.7 / §8.2.3 / plan_ld2b3 T18

#include <stddef.h>
#include <stdint.h>
#include <sys/link_map.h>
#include <xos/elf.h>

// global link_map list head (libc.so reads it via extern in
// collect_tls_from_link_map) visibility("default"): ld.so is globally
// -fvisibility=hidden, only this symbol needs to be exported to libc.so
__attribute__((visibility("default"))) struct link_map *_dl_link_map = NULL;

// ld.so itself: keep a single static node (no external dependencies, avoids one
// malloc)
struct link_map g_ld_map_static;

// parse .dynamic to fill link_map's symbol lookup + TLS fields
__attribute__((visibility("hidden"))) void
fill_link_map(struct link_map *l, uintptr_t base, Elf64_Dyn *dyn) {
  l->base = base;
  l->dynamic = dyn;

  // default zeroing
  l->symtab = NULL;
  l->strtab = NULL;
  l->gnu_hash = NULL;
  l->rela_dyn = NULL;
  l->rela_dyn_sz = 0;
  l->rela_plt = NULL;
  l->rela_plt_sz = 0;
  l->tls_template = NULL;
  l->tls_tdata_size = 0;
  l->tls_tbss_size = 0;
  l->tls_align = 0;

  if (!dyn)
    return;

  // first pass: find DT_STRTAB/DT_SYMTAB/DT_GNU_HASH (addresses need base
  // added; .so is PIC) note: main ELF is non-PIE, d_ptr is already an absolute
  // address; libc.so/ld.so is PIC, d_ptr is a relative vaddr needing base
  // simplification: try base + d_ptr for all objects (main ELF base=0 degrades
  // to absolute address)
  for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
    switch (d->d_tag) {
    case DT_STRTAB:
      l->strtab = (const char *)(base + d->d_un.d_ptr);
      break;
    case DT_SYMTAB:
      l->symtab = (void *)(base + d->d_un.d_ptr);
      break;
    case DT_GNU_HASH:
      l->gnu_hash = (void *)(base + d->d_un.d_ptr);
      break;
    }
  }

  // second pass: find DT_RELA/DT_RELASZ/DT_JMPREL/DT_PLTRELSZ + PT_TLS
  // PT_TLS info is not delivered via .dynamic; needs PHDR walk - but ld.so no
  // longer has the main ELF PHDR at this point simplification: TLS info is
  // filled by a separate PHDR walk at fill time (caller passes phdr) here we
  // only fill fields that .dynamic can provide
  for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
    switch (d->d_tag) {
    case DT_RELA:
      l->rela_dyn = (void *)(base + d->d_un.d_ptr);
      break;
    case DT_RELASZ:
      l->rela_dyn_sz = d->d_un.d_val;
      break;
    case DT_JMPREL:
      l->rela_plt = (void *)(base + d->d_un.d_ptr);
      break;
    case DT_PLTRELSZ:
      l->rela_plt_sz = d->d_un.d_val;
      break;
    }
  }
}

// find PT_TLS from PHDR, fill link_map's tls_* fields
// base is the load base (PIC .so adds offset; main ELF base=0)
__attribute__((visibility("hidden"))) void
fill_tls_from_phdr(struct link_map *l, uintptr_t base, uintptr_t phdr,
                   size_t phent, size_t phnum) {
  for (size_t i = 0; i < phnum; i++) {
    Elf64_Phdr *p = (Elf64_Phdr *)(phdr + i * phent);
    if (p->p_type == PT_TLS) {
      l->tls_template = (void *)(base + p->p_vaddr);
      l->tls_tdata_size = p->p_filesz;
      l->tls_tbss_size = p->p_memsz - p->p_filesz;
      l->tls_align = p->p_align;
      return;
    }
  }
}

/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// ld.so relocation: 10 types (9 PIC + COPY) + eager binding + diagnostic
// hard-fail ld.md §3.3.4, §3.3.6, §5.4 / plan_ld2b3 T16

#include <stddef.h>
#include <stdint.h>
#include <sys/link_map.h>
#include <xos/elf.h>
#include <xos/syscall_nums.h>

// bootstrap stage helpers (hidden)
__attribute__((visibility("hidden"))) void dl_puts(const char *s);
__attribute__((visibility("hidden"))) void dl_put_hex(uint64_t val);

// symtab.c
__attribute__((visibility("hidden"))) void *
lookup_symbol_in_link_map(const char *name, struct link_map *lmap);
__attribute__((visibility("hidden"))) Elf64_Sym *
lookup_symbol_def(const char *name, struct link_map *lmap,
                  struct link_map **out_def_map);

// minilibc.c
__attribute__((visibility("hidden"))) void *memcpy(void *dst, const void *src,
                                                   unsigned long n);

// get symbol name: symtab[sym_idx].st_name + strtab
static const char *sym_name(struct link_map *l, uint32_t sym_idx) {
  Elf64_Sym *symtab = (Elf64_Sym *)l->symtab;
  const char *strtab = l->strtab;
  return strtab + symtab[sym_idx].st_name;
}

// symbol lookup walks from global scope head (_dl_link_map = main ELF),
// consistent with eager_bind. list order main -> ld -> libs: if we walked
// forward starting from the relocated object lmap, we would miss ld.so symbols
// ahead of it (e.g. _dl_link_map), so we must look up from head. (R_X86_64_COPY
// is still special: lookup starts from lmap->l_next, skipping the main ELF's
// own .bss copy)
static void *lookup_global(const char *name) {
  return lookup_symbol_in_link_map(name, _dl_link_map);
}

// apply a single relocation
void apply_relocation(Elf64_Rela *r, void *base, struct link_map *lmap) {
  uint32_t type = ELF64_R_TYPE(r->r_info);
  uint32_t sym_idx = ELF64_R_SYM(r->r_info);
  uintptr_t *addr = (uintptr_t *)((char *)base + r->r_offset);
  int64_t addend = r->r_addend;

  switch (type) {
  case R_X86_64_RELATIVE:
    *addr = (uintptr_t)base + addend;
    break;

  case R_X86_64_COPY: {
    // only non-PIE main ELF references writable libc.so globals
    // (errno/stdout/stdin/stderr etc.) semantics: copy sym.st_size bytes from
    // the definer (libc.so) into the main ELF .bss (at r_offset) the main ELF
    // holds an independent copy; subsequent references inside the main ELF
    // resolve to its own copy, isolated from libc.so
    const char *name = sym_name(lmap, sym_idx);
    // look up starting from lmap->l_next (skip main ELF itself, avoid
    // self-referential memcpy)
    struct link_map *def_map = NULL;
    Elf64_Sym *def_sym = lookup_symbol_def(name, lmap->l_next, &def_map);
    if (!def_sym || !def_map)
      goto unresolved;
    void *src = (char *)def_map->base + def_sym->st_value;
    memcpy(addr, src, def_sym->st_size);
    break;
  }

  case R_X86_64_64: {
    const char *name = sym_name(lmap, sym_idx);
    void *sym = lookup_global(name);
    if (!sym)
      goto unresolved;
    *addr = (uintptr_t)sym + addend;
    break;
  }

  case R_X86_64_PC32:
  case R_X86_64_PLT32: {
    const char *name = sym_name(lmap, sym_idx);
    void *sym = lookup_global(name);
    if (!sym)
      goto unresolved;
    *(uint32_t *)addr = (uint32_t)((uintptr_t)sym + addend - (uintptr_t)addr);
    break;
  }

  case R_X86_64_GOTPCREL: {
    // plan_ld2b3 decision 6: two-step but **do not modify instruction disp**
    // instruction form: mov foo@GOTPCREL(%rip), %reg
    // the disp field is the 4 bytes before the instruction end; r_offset points
    // to the disp field instr_end = r_offset + 4 (address after disp = next
    // instruction address) got_entry = instr_end + disp (RIP-relative
    // computation) fill *got_entry = sym, leave disp unchanged (keeps
    // RIP-relative addressing correct)
    const char *name = sym_name(lmap, sym_idx);
    void *sym = lookup_global(name);
    if (!sym)
      goto unresolved;
    // read static disp from instruction (disp field immediately follows
    // r_offset position, 4 bytes)
    int32_t disp = *(int32_t *)addr;
    uintptr_t instr_end = (uintptr_t)addr + 4;
    uintptr_t got_entry = instr_end + disp;
    *(uintptr_t *)got_entry = (uintptr_t)sym;
    // disp unchanged
    break;
  }

  case R_X86_64_GLOB_DAT:
  case R_X86_64_JUMP_SLOT: {
    const char *name = sym_name(lmap, sym_idx);
    void *sym = lookup_global(name);
    if (!sym)
      goto unresolved;
    *addr = (uintptr_t)sym;
    break;
  }

  case R_X86_64_32: {
    const char *name = sym_name(lmap, sym_idx);
    void *sym = lookup_global(name);
    if (!sym)
      goto unresolved;
    *(uint32_t *)addr = (uint32_t)((uintptr_t)sym + addend);
    break;
  }

  case R_X86_64_32S: {
    const char *name = sym_name(lmap, sym_idx);
    void *sym = lookup_global(name);
    if (!sym)
      goto unresolved;
    *(int32_t *)addr = (int32_t)((uintptr_t)sym + addend);
    break;
  }

  default:
    dl_puts("dl: FATAL: unknown reloc type ");
    dl_put_hex(type);
    dl_puts(" @base ");
    dl_put_hex((uint64_t)base);
    dl_puts(" off ");
    dl_put_hex(r->r_offset);
    if (sym_idx) {
      dl_puts(" sym '");
      dl_puts(sym_name(lmap, sym_idx));
      dl_puts("'");
    }
    // TLS relocation types (16-23, 35-36): triggered by __thread variables
    // inside shared libs ld.so does not yet implement __tls_get_addr / TLS
    // descriptor; must use TCB fields
    if (type >= 16 && type <= 23) {
      dl_puts(
          " [TLS reloc: ld.so no __tls_get_addr; use TCB field, not __thread]");
    } else if (type == 35 || type == 36) {
      dl_puts(
          " [TLSDESC: ld.so no TLS descriptor; use TCB field, not __thread]");
    }
    dl_puts("\n");
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(SYS_EXIT), "D"(1)
                     : "rcx", "r11");
    while (1)
      ;
  }
  return;

unresolved:
  dl_puts("dl: FATAL: unresolved symbol '");
  dl_puts(sym_name(lmap, sym_idx));
  dl_puts("' @base ");
  dl_put_hex((uint64_t)base);
  dl_puts(" off ");
  dl_put_hex(r->r_offset);
  dl_puts("\n");
  long ret;
  __asm__ volatile("syscall"
                   : "=a"(ret)
                   : "a"(SYS_EXIT), "D"(1)
                   : "rcx", "r11");
  while (1)
    ;
}

// eager binding: walk .rela.plt, immediately resolve each JUMP_SLOT
void eager_bind(struct link_map *l) {
  Elf64_Rela *plt_rela = (Elf64_Rela *)l->rela_plt;
  size_t plt_sz = l->rela_plt_sz;
  if (!plt_rela || plt_sz == 0)
    return;

  for (size_t i = 0; i < plt_sz / sizeof(Elf64_Rela); i++) {
    Elf64_Rela *r = &plt_rela[i];
    if (ELF64_R_TYPE(r->r_info) != R_X86_64_JUMP_SLOT)
      continue;

    uint32_t sym_idx = ELF64_R_SYM(r->r_info);
    const char *name = sym_name(l, sym_idx);
    // look up from predecessor list (main ELF first, then libc.so, then ld.so)
    void *sym = lookup_symbol_in_link_map(name, _dl_link_map);
    if (!sym) {
      dl_puts("dl: FATAL: unresolved PLT symbol '");
      dl_puts(name);
      dl_puts("' @base ");
      dl_put_hex((uint64_t)l->base);
      dl_puts("\n");
      long ret;
      __asm__ volatile("syscall"
                       : "=a"(ret)
                       : "a"(SYS_EXIT), "D"(1)
                       : "rcx", "r11");
      while (1)
        ;
    }

    uintptr_t *got_entry = (uintptr_t *)((char *)l->base + r->r_offset);
    *got_entry = (uintptr_t)sym;
  }
}

// _dl_link_map declaration (defined in link_map.c)
extern struct link_map *_dl_link_map;

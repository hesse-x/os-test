/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// ld.so bootstrap entry
// ld.md §3.2.3 / plan_ld2b3 T13

#include <stddef.h>
#include <stdint.h>
#include <xos/elf.h>
#include <xos/syscall_nums.h>

// linker-provided .dynamic start symbol
// use asm to get RIP-relative address, avoiding GOT (GOT not filled before bootstrap)
extern Elf64_Dyn _DYNAMIC[];
// bootstrap stage helper declarations (hidden: visible across files but not via PLT, GOT
// not filled before bootstrap)
__attribute__((visibility("hidden"))) void dl_puts(const char *s);
__attribute__((visibility("hidden"))) void dl_put_hex(uint64_t val);

// post-bootstrap main flow (dls_init.c)
__attribute__((visibility("hidden"))) void __dls_init(uintptr_t *sp,
                                                      uintptr_t ld_base);

// get runtime address of _DYNAMIC (RIP-relative, does not depend on GOT)
static Elf64_Dyn *get_dynamic(void) {
  Elf64_Dyn *p;
  __asm__("leaq _DYNAMIC(%%rip), %0" : "=r"(p));
  return p;
}

// global pointer (triggers R_X86_64_RELATIVE relocation, verifies that after
// self-relocation it points to ld_base + offset)
// not used, only exists to make .rela.dyn non-empty so the bootstrap path
// actually performs relocation
static void *self_ptr = (void *)dl_puts;

// get value of given type from auxv
static uintptr_t find_auxv(uintptr_t *sp, uint64_t type) {
  // sp points to argc
  int argc = (int)sp[0];
  uintptr_t *argv = sp + 1;
  uintptr_t *envp = argv + argc + 1; // skip argv + NULL
  // walk envp to find NULL
  uintptr_t *p = envp;
  while (*p)
    p++;
  p++; // skip envp's NULL terminator
  // p now points to auxv
  while (*p != AT_NULL) {
    if (p[0] == type)
      return p[1];
    p += 2;
  }
  return 0;
}

// bootstrap entry
void __dls3(uintptr_t *sp) {
  // 1. get AT_BASE from auxv (own base address)
  uintptr_t ld_base = find_auxv(sp, AT_BASE);

  // 2. locate .dynamic (RIP-relative directly gets runtime address, no GOT)
  Elf64_Dyn *dyn = get_dynamic();

  // 3. first walk .dynamic raw to find DT_RELA/DT_RELASZ/DT_SYMTAB
  //    d_ptr is link-time relative vaddr, add ld_base when used
  Elf64_Rela *rela = NULL;
  size_t rela_sz = 0;
  Elf64_Sym *symtab = NULL;
  for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
    switch (d->d_tag) {
    case DT_RELA:
      rela = (Elf64_Rela *)(ld_base + d->d_un.d_ptr);
      break;
    case DT_RELASZ:
      rela_sz = d->d_un.d_val;
      break;
    case DT_SYMTAB:
      symtab = (Elf64_Sym *)(ld_base + d->d_un.d_ptr);
      break;
    }
  }

  // 4. walk .rela.dyn applying R_X86_64_RELATIVE and R_X86_64_GLOB_DAT
  //    RELATIVE:   *addr = ld_base + addend
  //    GLOB_DAT:   *addr = ld_base + symtab[sym].st_value + addend
  //    (symbols defined in ld.so itself; st_value is link-time vaddr, add base
  //    to get runtime address)
  //    GOT not filled before bootstrap; GLOB_DAT must be handled here, otherwise
  //    build_link_map reads the GOT entry of _dl_link_map as 0 and dereferences
  //    it -> #PF
  if (rela && rela_sz) {
    for (size_t i = 0; i < rela_sz / sizeof(Elf64_Rela); i++) {
      Elf64_Rela *r = &rela[i];
      uintptr_t *addr = (uintptr_t *)(ld_base + r->r_offset);
      uint32_t type = ELF64_R_TYPE(r->r_info);
      if (type == R_X86_64_RELATIVE) {
        *addr = ld_base + r->r_addend;
      } else if (type == R_X86_64_GLOB_DAT) {
        uint32_t sym_idx = ELF64_R_SYM(r->r_info);
        if (symtab && sym_idx) {
          *addr = ld_base + symtab[sym_idx].st_value + r->r_addend;
        }
      } else {
        // WARN on unhandled types: bootstrap only does RELATIVE/GLOB_DAT,
        // other types (e.g. JUMP_SLOT/PC32) mean ld.so has intra-module calls,
        // and the unfilled GOT before bootstrap would crash. Normally ld.so is
        // built with global -fvisibility=hidden so .rela.dyn only contains
        // RELATIVE/GLOB_DAT
        dl_puts("dl: WARN: skip reloc type ");
        dl_put_hex(type);
        dl_puts(" at offset ");
        dl_put_hex(r->r_offset);
        dl_puts("\n");
      }
    }
  }

  // 5. self-relocation done
  dl_puts("dl: self-relocate done");

  // 6. enter post-bootstrap main flow (global variables/GOT now usable)
  __dls_init(sp, ld_base);
  // __dls_init does not return (jumps to main ELF entry)
  while (1)
    ;
}

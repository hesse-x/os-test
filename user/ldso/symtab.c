/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// ld.so symbol lookup: GNU hash + list walk
// ld.md §3.3.5 / plan_ld2b3 T17

#include <stddef.h>
#include <stdint.h>
#include <sys/link_map.h>
#include <xos/elf.h>
#include <xos/syscall_nums.h>

// minilibc.c
__attribute__((visibility("hidden"))) int strcmp(const char *a, const char *b);

// GNU hash function
static uint32_t gnu_hash(const char *s) {
  uint32_t h = 5381;
  while (*s)
    h = (h << 5) + h + (uint8_t)(*s++);
  return h;
}

// GNU hash lookup: returns the Elf64_Sym * in symtab (including
// st_value/st_size), or NULL if not found. caller must add link_map.base to get
// the runtime absolute address
static Elf64_Sym *gnu_hash_lookup(const char *name, Elf64_Sym *symtab,
                                  const char *strtab,
                                  uint32_t *gnu_hash_table) {
  if (!gnu_hash_table)
    return NULL;

  uint32_t nbuckets = gnu_hash_table[0];
  uint32_t symoffset = gnu_hash_table[1];
  uint32_t bloom_size = gnu_hash_table[2];
  uint32_t bloom_shift = gnu_hash_table[3];

  if (nbuckets == 0 || bloom_size == 0)
    return NULL;

  uint32_t h = gnu_hash(name);

  // bloom filter quick reject
  uint64_t *bloom = (uint64_t *)&gnu_hash_table[4];
  uint64_t word = bloom[(h / 64) % bloom_size];
  uint64_t mask = (1ULL << (h % 64)) | (1ULL << ((h >> bloom_shift) % 64));
  if ((word & mask) != mask)
    return NULL;

  // bucket finds chain head
  uint32_t *buckets = (uint32_t *)((char *)bloom + bloom_size * 8);
  uint32_t symidx = buckets[h % nbuckets];
  if (symidx == 0)
    return NULL; // empty bucket

  // chain walk
  uint32_t *chain = buckets + nbuckets;
  uint32_t chain_h;
  do {
    chain_h = chain[symidx - symoffset];
    if ((h | 1) == (chain_h | 1)) { // ignore lowest bit (end marker)
      Elf64_Sym *sym = &symtab[symidx];
      if (strcmp(strtab + sym->st_name, name) == 0) {
        return sym;
      }
    }
    symidx++;
  } while ((chain_h & 1) == 0); // lowest bit 1 means chain end

  return NULL;
}

// walk link_map to look up symbol (list order: main ELF -> libc.so -> ld.so)
// returns absolute address (base already added), or NULL if not found
void *lookup_symbol_in_link_map(const char *name, struct link_map *lmap) {
  for (struct link_map *l = lmap; l; l = l->l_next) {
    Elf64_Sym *sym = gnu_hash_lookup(name, (Elf64_Sym *)l->symtab, l->strtab,
                                     (uint32_t *)l->gnu_hash);
    if (sym)
      return (char *)l->base + (uintptr_t)sym->st_value;
  }
  return NULL;
}

// for COPY relocation: find symbol definer, returns definer's Elf64_Sym *
// (including st_value/st_size) out_def_map receives the definer's link_map,
// used to get base; returns NULL if not found only used for main ELF's
// R_X86_64_COPY: caller looks up starting from lmap->l_next (skip main ELF
// itself, otherwise the same-named symbol pending fill in main ELF .bss would
// be found, making self-referential memcpy meaningless)
__attribute__((visibility("hidden"))) Elf64_Sym *
lookup_symbol_def(const char *name, struct link_map *lmap,
                  struct link_map **out_def_map) {
  for (struct link_map *l = lmap; l; l = l->l_next) {
    Elf64_Sym *sym = gnu_hash_lookup(name, (Elf64_Sym *)l->symtab, l->strtab,
                                     (uint32_t *)l->gnu_hash);
    if (sym) {
      if (out_def_map)
        *out_def_map = l;
      return sym;
    }
  }
  return NULL;
}

// _dl_link_map declaration (defined in link_map.c)
extern struct link_map *_dl_link_map;
